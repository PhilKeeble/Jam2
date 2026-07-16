#include "CliEntrypoint.hpp"

#include "CliOptions.hpp"
#include "CliRuntime.hpp"
#include "audio_device.hpp"

#include <exception>
#include <iostream>
#include <string_view>

namespace {

int run(int argc, char** argv)
{
    using namespace jam2::cli;

    if (argc <= 1) {
        std::cout << kUsage;
        return 0;
    }

    const std::string_view command{argv[1]};
    if (command == "--help" || command == "-h" || command == "help") {
        std::cout << kUsage;
        return 0;
    }

    if (command == "list-devices") {
        if (has_help_argument(argc, argv, 2)) {
            print_device_help(command);
            return 0;
        }
        const auto devices = jam2::audio::list_devices();
        if (devices.empty()) {
            std::cout << "No audio devices found for this platform backend.\n";
            return 0;
        }
        for (const auto& device : devices) {
            std::cout << "[" << device.id << "] " << device.backend << " " << device.name << "\n";
        }
        return 0;
    }

    if (command == "test-device") {
        if (has_help_argument(argc, argv, 2)) {
            print_device_help(command);
            return 0;
        }
        return runTestDevice(argc, argv);
    }

    if (command == "local") {
        if (has_help_argument(argc, argv, 2)) {
            print_local_help();
            return 0;
        }
        return runLocal(argc, argv);
    }

    if (command == "network") {
        if (argc < 3 || is_help_argument(argv[2])) {
            std::cout << kNetworkUsage;
            return 0;
        }
        const std::string_view operation{argv[2]};
        if (operation == "create") {
            if (has_help_argument(argc, argv, 3)) {
                print_network_create_help();
                return 0;
            }
            throw std::runtime_error("network create bootstrap is owned by the unified Jam2 application");
        }
        if (operation == "join") {
            if (has_help_argument(argc, argv, 3)) {
                print_network_join_help();
                return 0;
            }
            throw std::runtime_error("network join bootstrap is owned by the unified Jam2 application");
        }
        std::cerr << "Unknown network subcommand: " << operation << "\n\n" << kNetworkUsage;
        return 2;
    }

    std::cerr << "Unknown command: " << command << "\n\n" << kUsage;
    return 2;
}

} // namespace

int jam2::cli::runFrontend(int argc, char** argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "fatal: unknown error\n";
        return 1;
    }
}
