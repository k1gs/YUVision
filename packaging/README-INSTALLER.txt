YUVision Windows Installer
==========================

Recommended installation:
  Run Install-YUVision.cmd and approve the Windows administrator prompt.

The command wrapper installs the adjacent MSI and writes a detailed log to:
  logs\YUVision-install.log

You may double-click the MSI directly, but that path does not automatically create a verbose
diagnostic log next to the package. All application files required by YUVision are embedded in
the MSI. The Microsoft C++ runtime is linked statically and does not need a separate download.

Uninstallation:
  Use Windows Settings > Apps > Installed apps, or run Uninstall-YUVision.cmd from this folder.

Application logs:
  %LOCALAPPDATA%\YUVision\Logs\YUVision.log
  %LOCALAPPDATA%\YUVision\Logs\YUVision.previous.log

YUVision can open this folder from its Open logs menu item.
