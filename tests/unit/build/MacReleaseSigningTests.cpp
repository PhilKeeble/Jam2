#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
QByteArray read(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "fixture artifact is readable");
    return file.readAll();
}
void write(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size(),
        "fixture artifact can be written");
}
struct Result { int code; QByteArray output; };
Result run(const QString& program, const QStringList& arguments)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    require(process.waitForStarted(10000), "release flow process starts");
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished();
        throw std::runtime_error("release flow process timed out");
    }
    require(process.exitStatus() == QProcess::NormalExit, "release flow process exits normally");
    return {process.exitCode(), process.readAll()};
}

// Replace external signing/notary services, while running the actual packager,
// generated release hook, filesystem checks, manifest generation and ditto ZIP.
constexpr auto backend = R"cmake(
cmake_minimum_required(VERSION 3.24)
set(root "${JAM2_RELEASE_WORK}/release")
set(app "${root}/Jam2.app")
file(MAKE_DIRECTORY "${app}/Contents/MacOS" "${app}/Contents/Helpers"
    "${app}/Contents/Frameworks/Fixture.framework/Versions/A" "${JAM2_RELEASE_WORK}/qt")
file(WRITE "${JAM2_RELEASE_WORK}/qt/macdeployqt" "fake deployment tool")
foreach(path IN ITEMS "Contents/MacOS/jam2" "Contents/Helpers/jam2-plugin-worker"
        "Contents/Helpers/jamtaster-worker" "Contents/Helpers/jam2-updater"
        "Contents/Frameworks/Fixture.framework/Versions/A/Fixture")
    file(WRITE "${app}/${path}" "unsigned fixture")
endforeach()
file(WRITE "${JAM2_PACKAGE_CONFIG}" "set(JAM2_PACKAGE_SOURCE [==[${JAM2_RELEASE_WORK}]==])\n"
    "set(JAM2_PACKAGE_RELEASE [==[${root}]==])\nset(JAM2_PACKAGE_VERSION 1.2.3)\n"
    "set(JAM2_PACKAGE_PLATFORM macos)\nset(JAM2_PACKAGE_ARCHITECTURES arm64 x86_64)\n"
    "set(JAM2_PACKAGE_APP_FILES Jam2.app)\nset(JAM2_PACKAGE_MODEL_NAMES)\n"
    "set(JAM2_PACKAGE_QT_BIN [==[${JAM2_RELEASE_WORK}/qt]==])\n")
function(execute_process)
    string(JOIN " " invocation ${ARGV})
    file(APPEND "${JAM2_RELEASE_WORK}/trace" "${invocation}\n")
    set(output "")
    set(result 0)
    if(invocation MATCHES "/codesign --force")
        list(FIND ARGV "--sign" sign_index)
        math(EXPR sign_index "${sign_index} + 1")
        list(GET ARGV ${sign_index} identity)
        list(FIND ARGV "COMMAND_ERROR_IS_FATAL" target_index)
        math(EXPR target_index "${target_index} - 1")
        list(GET ARGV ${target_index} target)
        if(NOT identity STREQUAL "-")
            if(NOT invocation MATCHES "--timestamp --options runtime")
                message(FATAL_ERROR "Signing requires timestamp and hardened runtime")
            endif()
            if(invocation MATCHES "jam2-plugin-worker")
                if(NOT invocation MATCHES "--entitlements .*plugin-host.plist")
                    message(FATAL_ERROR "Plugin host entitlement missing")
                endif()
                file(READ "${JAM2_RELEASE_WORK}/plugin-host.plist" entitlements)
                if(NOT entitlements MATCHES "com.apple.security.cs.disable-library-validation")
                    message(FATAL_ERROR "Foreign-library loading entitlement missing")
                endif()
            elseif(target STREQUAL app)
                if(NOT invocation MATCHES "--entitlements .*audio-input.plist")
                    message(FATAL_ERROR "Main app entitlement missing")
                endif()
                file(READ "${JAM2_RELEASE_WORK}/audio-input.plist" entitlements)
                if(NOT entitlements MATCHES "com.apple.security.device.audio-input")
                    message(FATAL_ERROR "Audio input entitlement missing")
                endif()
            endif()
        endif()
        file(APPEND "${JAM2_RELEASE_WORK}/signatures" "${target}\n")
        if(case STREQUAL "sign-failure")
            message(FATAL_ERROR "Fixture signing failure")
        endif()
        # Make signing change payload bytes so the final manifest must be fresh.
        if(target STREQUAL app)
            file(WRITE "${app}/Contents/MacOS/jam2" "signed fixture")
        endif()
    elseif(invocation MATCHES "/macdeployqt")
        if(NOT invocation MATCHES "-executable=${app}/Contents/Helpers/jam2-updater")
            message(FATAL_ERROR "Updater dependency deployment missing")
        endif()
    elseif(invocation MATCHES "/codesign --verify")
        # Signature validation is represented by the recorded event.
    elseif(invocation MATCHES "/file -b")
        set(output "Mach-O")
    elseif(invocation MATCHES "/lipo" OR invocation MATCHES "/otool")
        # Universal portable dependency fixtures.
    elseif(invocation MATCHES "Contents/MacOS/jam2 --version")
        set(output "Jam2 1.2.3")
    elseif(invocation MATCHES "notarytool submit")
        if(case STREQUAL "transport")
            set(result 1)
        elseif(case STREQUAL "malformed")
            set(output "invalid JSON")
        elseif(case STREQUAL "missing-id")
            set(output "{\"status\":\"Accepted\"}")
        elseif(case STREQUAL "rejected")
            set(output "{\"id\":\"fixture-id\",\"status\":\"Invalid\"}")
        else()
            set(output "{\"id\":\"fixture-id\",\"status\":\"Accepted\"}")
        endif()
    elseif(invocation MATCHES "notarytool log")
        if(case STREQUAL "log-failure")
            set(result 1)
        else()
            file(WRITE "${JAM2_PACKAGE_OUTPUT}/Jam2-notarization-log.json" "{\"issues\":[]}")
        endif()
    elseif(invocation MATCHES "stapler staple")
        if(case STREQUAL "staple-failure")
            message(FATAL_ERROR "Fixture staple failure")
        endif()
        file(WRITE "${app}/Contents/ticket" "stapled ticket")
    elseif(invocation MATCHES "stapler validate")
        if(case STREQUAL "validate-failure")
            message(FATAL_ERROR "Fixture ticket validation failure")
        endif()
    elseif(invocation MATCHES "/spctl")
        if(case STREQUAL "gatekeeper-failure")
            message(FATAL_ERROR "Fixture Gatekeeper failure")
        endif()
    else()
        _execute_process(${ARGV})
        return()
    endif()
    foreach(keyword IN ITEMS OUTPUT_VARIABLE RESULT_VARIABLE ERROR_VARIABLE)
        list(FIND ARGV "${keyword}" index)
        if(NOT index EQUAL -1)
            math(EXPR index "${index} + 1")
            list(GET ARGV ${index} variable)
            if(keyword STREQUAL "RESULT_VARIABLE")
                set("${variable}" "${result}" PARENT_SCOPE)
            elseif(keyword STREQUAL "OUTPUT_VARIABLE")
                set("${variable}" "${output}" PARENT_SCOPE)
            else()
                set("${variable}" "" PARENT_SCOPE)
            endif()
        endif()
    endforeach()
endfunction()
include("${package_script}")
)cmake";
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        require(argc == 5, "expected CMake, release shell script, packager and workspace");
        const QString cmake = QString::fromLocal8Bit(argv[1]);
        const QByteArray shell = read(QString::fromLocal8Bit(argv[2]));
        const QByteArray marker("<<'CMAKE'\n");
        const auto start = shell.indexOf(marker);
        const auto end = shell.indexOf("\nCMAKE\n", start);
        require(start >= 0 && end > start, "release script contains its actual CMake finalization hook");
        const QByteArray hook = shell.mid(start + marker.size(), end - start - marker.size());
        const QString packager = QString::fromLocal8Bit(argv[3]);
        const QString workspace = QString::fromLocal8Bit(argv[4]);
        require(QDir().mkpath(workspace), "release test workspace exists");
        QTemporaryDir temporary(QDir(workspace).filePath(QStringLiteral("mac signing-XXXXXX")));
        require(temporary.isValid(), "release fixtures have a private directory with spaces");
        const QString hookPath = temporary.filePath(QStringLiteral("finalize.cmake"));
        const QString driver = temporary.filePath(QStringLiteral("driver.cmake"));
        write(hookPath, hook);
        write(driver, backend);
        for (const QString& scenario : {QStringLiteral("accepted"), QStringLiteral("signed-only"),
                QStringLiteral("adhoc"), QStringLiteral("sign-failure"), QStringLiteral("rejected"), QStringLiteral("transport"),
                QStringLiteral("malformed"), QStringLiteral("missing-id"), QStringLiteral("log-failure"),
                QStringLiteral("staple-failure"), QStringLiteral("validate-failure"), QStringLiteral("gatekeeper-failure")}) {
            const QString work = temporary.filePath(scenario);
            require(QDir().mkpath(work), "scenario workspace exists");
            const bool adhoc = scenario == QStringLiteral("adhoc");
            const bool signedOnly = scenario == QStringLiteral("signed-only");
            const bool success = adhoc || signedOnly || scenario == QStringLiteral("accepted");
            const auto result = run(cmake, {QStringLiteral("-DJAM2_RELEASE_WORK=") + work,
                QStringLiteral("-DJAM2_PACKAGE_CONFIG=") + work + QStringLiteral("/config.cmake"),
                QStringLiteral("-DJAM2_PACKAGE_OUTPUT=") + work + QStringLiteral("/dist"),
                QStringLiteral("-DJAM2_SIGN_IDENTITY=") + (adhoc ? QStringLiteral("-") : QStringLiteral("Developer ID Application: Fixture")),
                QStringLiteral("-DJAM2_NOTARY_PROFILE=") + (adhoc || signedOnly ? QString() : QStringLiteral("OFF")),
                QStringLiteral("-DJAM2_MACOS_FINALIZE_SCRIPT=") + (adhoc ? QString() : hookPath),
                QStringLiteral("-Dpackage_script=") + packager, QStringLiteral("-Dcase=") + scenario,
                QStringLiteral("-P"), driver});
            if ((result.code == 0) != success) std::cerr << result.output.constData();
            require((result.code == 0) == success, "release success follows signing/notarization result");
            const auto signatures = read(work + QStringLiteral("/signatures")).trimmed().split('\n');
            const QSet<QByteArray> unique(signatures.begin(), signatures.end());
            const int signedCount = scenario == QStringLiteral("sign-failure") ? 1 : 5;
            require(signatures.size() == signedCount && unique.size() == signedCount,
                "each nested target and the enclosing app are signed exactly once");
            if (signedCount == 5) {
                require(unique.contains((work + QStringLiteral("/release/Jam2.app/Contents/Helpers/jam2-updater")).toUtf8()),
                    "the merged updater helper participates in the single signing pass");
                require(signatures.last() == (work + QStringLiteral("/release/Jam2.app")).toUtf8(),
                    "the enclosing app is signed after all nested code");
            }
            const QByteArray trace = read(work + QStringLiteral("/trace"));
            const auto zipPosition = trace.indexOf("Jam2-macos.zip.partial");
            require((zipPosition >= 0) == success, "no final ZIP is attempted before all checks succeed");
            require(QFile::exists(work + QStringLiteral("/dist/Jam2-macos.zip")) == success &&
                    QFile::exists(work + QStringLiteral("/dist/Jam2-macos.zip.json")) == success,
                "failed release produces neither final archive nor publication metadata");
            if (scenario == QStringLiteral("accepted")) {
                const auto submit = trace.indexOf("notarytool submit");
                const auto staple = trace.indexOf("stapler staple");
                const auto validate = trace.indexOf("stapler validate");
                const auto gatekeeper = trace.indexOf("/spctl");
                require(trace.lastIndexOf("/codesign --force") < submit && submit < staple &&
                        staple < validate && validate < gatekeeper && gatekeeper < zipPosition,
                    "signing, notarization, stapling, validation and final ZIP occur in order");
                require(trace.count("Jam2-macos.zip.partial") == 1,
                    "the distribution ZIP is created only once");
                const QString archive = work + QStringLiteral("/dist/Jam2-macos.zip");
                const auto metadata = QJsonDocument::fromJson(read(archive + QStringLiteral(".json"))).object();
                const auto zipBytes = read(archive);
                require(metadata.value(QStringLiteral("bytes")).toInteger() == zipBytes.size() &&
                        metadata.value(QStringLiteral("sha256")).toString().toLatin1() ==
                            QCryptographicHash::hash(zipBytes, QCryptographicHash::Sha256).toHex(),
                    "publication metadata matches the finished stapled archive");
                const auto manifest = QJsonDocument::fromJson(read(work +
                    QStringLiteral("/release/package-manifest.json"))).object();
                bool ticketFound = false;
                for (const auto& value : manifest.value(QStringLiteral("files")).toArray()) {
                    const auto entry = value.toObject();
                    const QString path = entry.value(QStringLiteral("path")).toString();
                    const auto bytes = read(work + QStringLiteral("/release/") + path);
                    require(entry.value(QStringLiteral("bytes")).toInteger() == bytes.size() &&
                            entry.value(QStringLiteral("sha256")).toString().toLatin1() ==
                                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
                        "manifest records bytes after signing and stapling");
                    ticketFound |= path == QStringLiteral("Jam2.app/Contents/ticket");
                }
                require(ticketFound, "stapled ticket enters the final manifest");
                const auto listing = run(cmake, {QStringLiteral("-E"), QStringLiteral("tar"),
                    QStringLiteral("tf"), work + QStringLiteral("/dist/Jam2-macos.zip")});
                require(listing.code == 0 && listing.output.contains("Jam2.app/Contents/ticket"),
                    "final ZIP contains the stapled ticket");
            } else if (adhoc || signedOnly) {
                require(!trace.contains("notarytool") && !trace.contains("stapler"),
                    "local signing paths do not submit to Apple");
            }
        }
        std::cout << "Mac release signing flow checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Mac release signing failure: " << error.what() << '\n';
        return 1;
    }
}
