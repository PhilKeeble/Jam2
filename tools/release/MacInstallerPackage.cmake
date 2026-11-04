# Included after the app has been deployed, signed and optionally notarized.
function(jam2_make_macos_installer)
    set(stage "${JAM2_PACKAGE_OUTPUT}/.macos-installer")
    set(destination "${JAM2_PACKAGE_OUTPUT}/Jam2-macos.pkg")
    if(IS_SYMLINK "${stage}" OR IS_SYMLINK "${destination}" OR IS_SYMLINK "${destination}.json")
        message(FATAL_ERROR "Installer destinations must not be symlinks")
    endif()
    file(REMOVE_RECURSE "${stage}")
    file(MAKE_DIRECTORY "${stage}/payload/Applications" "${stage}/resources")
    execute_process(COMMAND /usr/bin/ditto "${bundle}" "${stage}/payload/Applications/Jam2.app"
        COMMAND_ERROR_IS_FATAL ANY)
    file(WRITE "${stage}/components.plist" [=[<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><array><dict>
<key>RootRelativeBundlePath</key><string>Applications/Jam2.app</string>
<key>BundleIsRelocatable</key><false/>
<key>BundleIsVersionChecked</key><true/>
<key>BundleHasStrictIdentifier</key><true/>
<key>BundleOverwriteAction</key><string>upgrade</string>
</dict></array></plist>
]=])
    file(WRITE "${stage}/distribution.xml" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<installer-gui-script minSpecVersion=\"2\">
<title>Jam2 ${JAM2_PACKAGE_VERSION}</title>
<options customize=\"never\" require-scripts=\"false\"/>
<domains enable_localSystem=\"true\" enable_currentUserHome=\"false\" enable_anywhere=\"false\"/>
<conclusion file=\"conclusion.html\" mime-type=\"text/html\"/>
<choices-outline><line choice=\"jam2\"/></choices-outline>
<choice id=\"jam2\" visible=\"false\"><pkg-ref id=\"com.jam2.pkg\"/></choice>
<pkg-ref id=\"com.jam2.pkg\" version=\"${JAM2_PACKAGE_VERSION}\">Jam2.component.pkg</pkg-ref>
<pkg-ref id=\"com.jam2.pkg\"><must-close><app id=\"com.jam2.app\"/></must-close></pkg-ref>
</installer-gui-script>\n")
    file(WRITE "${stage}/resources/conclusion.html" "<!doctype html><html><body>
<h2>Jam2 is installed</h2><p>You can now open Jam2 from Applications.</p>
<p>Your settings, recordings, jams and downloaded models are stored separately
in your user Library/Application Support/Jam2 folder and are preserved during updates.</p>
</body></html>\n")
    execute_process(COMMAND /usr/bin/pkgbuild --root "${stage}/payload"
        --component-plist "${stage}/components.plist" --identifier com.jam2.pkg
        --version "${JAM2_PACKAGE_VERSION}" --install-location / --ownership recommended
        "${stage}/Jam2.component.pkg" COMMAND_ERROR_IS_FATAL ANY)
    set(sign_options "")
    if(JAM2_INSTALLER_SIGN_IDENTITY)
        if(NOT JAM2_SIGN_IDENTITY OR JAM2_SIGN_IDENTITY STREQUAL "-")
            message(FATAL_ERROR "Signing the PKG requires a Developer ID Application signature on Jam2.app too")
        endif()
        list(APPEND sign_options --sign "${JAM2_INSTALLER_SIGN_IDENTITY}" --timestamp)
    elseif(NOT "${JAM2_NOTARY_PROFILE}" STREQUAL "")
        message(FATAL_ERROR "A Developer ID Installer identity is required before notarizing a PKG")
    else()
        message(STATUS "Local test PKG is unsigned; it cannot be used by the public updater")
    endif()
    set(package "${stage}/Jam2-macos.pkg")
    execute_process(COMMAND /usr/bin/productbuild --distribution "${stage}/distribution.xml"
        --package-path "${stage}" --resources "${stage}/resources" ${sign_options} "${package}"
        COMMAND_ERROR_IS_FATAL ANY)
    if(JAM2_INSTALLER_SIGN_IDENTITY)
        execute_process(COMMAND /usr/sbin/pkgutil --check-signature "${package}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
    if(NOT "${JAM2_NOTARY_PROFILE}" STREQUAL "")
        execute_process(COMMAND /usr/bin/xcrun notarytool submit "${package}"
            --keychain-profile "${JAM2_NOTARY_PROFILE}" --wait --output-format json
            RESULT_VARIABLE submitted OUTPUT_VARIABLE result ERROR_VARIABLE detail)
        write_json("${JAM2_PACKAGE_OUTPUT}/Jam2-pkg-notarization.json" "${result}")
        string(JSON status ERROR_VARIABLE error GET "${result}" status)
        string(JSON identifier ERROR_VARIABLE id_error GET "${result}" id)
        if(id_error STREQUAL "NOTFOUND")
            execute_process(COMMAND /usr/bin/xcrun notarytool log "${identifier}"
                --keychain-profile "${JAM2_NOTARY_PROFILE}" "${JAM2_PACKAGE_OUTPUT}/Jam2-pkg-notarization-log.json"
                COMMAND_ERROR_IS_FATAL ANY)
        endif()
        if(NOT submitted EQUAL 0 OR NOT error STREQUAL "NOTFOUND" OR NOT id_error STREQUAL "NOTFOUND" OR NOT status STREQUAL "Accepted")
            message(FATAL_ERROR "Installer notarization failed (${status}): ${detail}")
        endif()
        execute_process(COMMAND /usr/bin/xcrun stapler staple "${package}" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND /usr/bin/xcrun stapler validate "${package}" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND /usr/sbin/spctl --assess --type install --verbose=2 "${package}"
            COMMAND_ERROR_IS_FATAL ANY)
    endif()
    file(RENAME "${package}" "${destination}")
    file(SIZE "${destination}" bytes)
    file(SHA256 "${destination}" hash)
    write_json("${destination}.json"
        "{\"schema\":1,\"channel\":\"stable\",\"version\":\"${JAM2_PACKAGE_VERSION}\",\"tag\":\"v${JAM2_PACKAGE_VERSION}\",\"asset\":\"Jam2-macos.pkg\",\"bytes\":${bytes},\"sha256\":\"${hash}\",\"url\":\"${repository_url}/v${JAM2_PACKAGE_VERSION}/Jam2-macos.pkg\",\"models_tag\":\"${model_tag}\",\"models_manifest_sha256\":\"${model_manifest_hash}\"}")
    file(REMOVE_RECURSE "${stage}")
    message(STATUS "Ready: ${destination} (${bytes} bytes, SHA-256 ${hash})")
endfunction()
