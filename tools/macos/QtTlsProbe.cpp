#include <QCoreApplication>
#include <QSslSocket>
#include <QTextStream>

#include <exception>

// Used by update.sh before a build exists. Keep this check offline and restrict
// plugin lookup to the selected SDK so another Qt cannot mask a broken install.
int main(int argc, char** argv)
{
    try {
        QCoreApplication app(argc, argv);
        if (argc != 2) return 2;
        QCoreApplication::setLibraryPaths({QString::fromLocal8Bit(argv[1])});
        if (!QSslSocket::supportsSsl()) {
            QTextStream(stderr) << "Qt has no usable TLS backend in " << argv[1] << Qt::endl;
            return 1;
        }
        QTextStream(stdout) << "Qt TLS backend: " << QSslSocket::activeBackend() << Qt::endl;
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << "Qt TLS check failed: " << error.what() << Qt::endl;
        return 1;
    }
}
