#include "ConnectionGuidance.hpp"

namespace jam2::gui {

QString creatorFirewallGuidance()
{
#if defined(__APPLE__)
    return QStringLiteral(
        "Jam2 accepted incoming TCP connections, but they closed before authentication began. "
        "macOS Firewall may be blocking Jam2.\n\n"
        "Open System Settings > Network > Firewall > Options and set Jam2 to Allow incoming "
        "connections, then ask the peer to retry.");
#elif defined(_WIN32)
    return QStringLiteral(
        "Jam2 accepted incoming TCP connections, but they closed before authentication began. "
        "Windows Firewall or other network security software on either computer may be blocking "
        "the connection.\n\n"
        "Open Windows Security > Firewall & network protection > Allow an app through firewall, "
        "allow Jam2 on the active network, then ask the peer to retry.");
#else
    return QStringLiteral(
        "Jam2 accepted incoming TCP connections, but they closed before authentication began. "
        "Check firewall and network security settings on both computers, then ask the peer to retry.");
#endif
}

QString joinerFirewallGuidance()
{
    return QStringLiteral(
        "\n\nNo authenticated TCP control connection was established. Confirm that the creator is "
        "still hosting and that the invite address is correct. Also check that Jam2 is allowed "
        "through macOS Firewall or Windows Firewall on the creator's computer and through any "
        "third-party network security software on both computers.");
}

} // namespace jam2::gui
