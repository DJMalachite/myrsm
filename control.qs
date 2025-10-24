function Controller()
{
    var targetBase = installer.value("ProductName");
    installer.setValue("TargetDir", "@HomeDir@/." + targetBase + "_app");
    if (installer.value("os") === "win") {
        installer.setValue("TargetDir", "@HomeDir@/AppData/Local/" + targetBase);
    } else if(installer.value("os") === "mac") {
        installer.setValue("TargetDir", "@HomeDir@/Applications/" + targetBase + ".app");
    }

    installer.setValue("allUsers", "false");
    installer.uninstallationStarted.connect(this, Controller.prototype.onUninstallationStarted);
    installer.uninstallationFinished.connect(this, Controller.prototype.onUninstallationFinished);
    installer.installationFinished.connect(this, Controller.prototype.onInstallationFinished);
}

function getBaseRegistryPath() {
    var path = installer.value("HKEY_LOCAL_MACHINE\\WOW6432Node\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + installer.value("ProductUUID") + "\\InstallLocation")
}

Controller.prototype.onUninstallationStarted = function () {
    var args  = [
        "stop",
        "MyRsm"
    ];
    console.log(args)
    var output = installer.execute("C:\\Windows\\SysWOW64\\sc.exe", args);
    console.log(output)
}
Controller.prototype.onUninstallationFinished = function () {
    console.log('Executing onUninstallationFinished')
    if (installer.value("os") === "win" )
    {
        var args  = [
            "delete",
            "MyRsm"
        ];
        console.log(args)
        var output = installer.execute("C:\\Windows\\SysWOW64\\sc.exe", args);
        console.log(output)
        var base = "WOW6432Node\\"
        var path = installer.value("HKEY_LOCAL_MACHINE\\SOFTWARE\\" + base + "Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + installer.value("ProductUUID") + "\\InstallLocation")
        if (path === "")
        {
            base = ""
            path = installer.value("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + installer.value("ProductUUID") + "\\InstallLocation")
        }
        console.log(installer.value("ProductUUID"))
        console.log(path)
        if (path !== "" && installer.gainAdminRights()) {
            console.log('Executing reg delete of the machine key.')
            var args  = [
                            "delete",
                            "HKEY_LOCAL_MACHINE\\SOFTWARE\\" + base + "Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + installer.value("ProductUUID"),
                            "/f"
                        ];
            var output = installer.execute("C:\\Windows\\SysWOW64\\reg.exe", args);
            console.log(output)
            console.log("done")
        } else {
            console.log("HKLM key not found, install was alluser = false")
        }
    }
}

Controller.prototype.onInstallationFinished = function() {
    //windows -> update the registry entry for the installer to work properly
    if (installer.value("os") === "win") {
        var args  = [
            "create",
            "MyRsm",
            "binPath=",
            installer.value("TargetDir") + "/bin/myrsm.exe",
            "start=",
            "auto"
        ];
        console.log(args)
        var output = installer.execute("C:\\Windows\\SysWOW64\\sc.exe", args);
        console.log(output)
        var args  = [
            "start",
            "MyRsm"
        ];
        console.log(args)
        var output = installer.execute("C:\\Windows\\SysWOW64\\sc.exe", args);
        console.log(output)
        if (installer.value("allUsers") === "true") {
            if (installer.gainAdminRights()) {
                var base = "WOW6432Node\\"
                var path = installer.value("HKEY_LOCAL_MACHINE\\SOFTWARE\\" + base + "Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + installer.value("ProductUUID") + "\\InstallLocation")
                if (path === "")
                {
                    base = ""
                    path = installer.value("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + installer.value("ProductUUID") + "\\InstallLocation")
                }
                console.log(path)
                console.log('Copying user key to local machine')
                var args  = [
                                "copy",
                                "HKEY_CURRENT_USER\\SOFTWARE\\" + base + "Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + installer.value("ProductUUID"),
                                "HKEY_LOCAL_MACHINE\\SOFTWARE\\" + base + "Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + installer.value("ProductUUID"),
                                "/f",
                                "/s"
                            ];
                var output = installer.execute("C:\\Windows\\SysWOW64\\reg.exe", args);
                console.log(output)
                console.log('Deleting user key')
                args  = [
                            "delete",
                            "HKEY_CURRENT_USER\\SOFTWARE\\" + base + "Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + installer.value("ProductUUID"),
                            "/f"
                        ];
                output = installer.execute("C:\\Windows\\SysWOW64\\reg.exe", args);
                console.log(output)
                console.log("done")
            }
        }
    }
}
