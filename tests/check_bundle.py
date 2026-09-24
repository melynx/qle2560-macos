"""Check the actual build artifact, including embedding and PCI match scope."""

import pathlib
import plistlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[1]
app = root / "build/DerivedData/Build/Products/Debug/QLE2560 Driver.app"
dext = app / "Contents/Library/SystemExtensions/org.iiyume.qle2560.driver.dext"


def read(path):
    return plistlib.loads(path.read_bytes())


app_info = read(app / "Contents/Info.plist")
driver_info = read(dext / "Info.plist")
assert app_info["CFBundleIdentifier"] == "org.iiyume.qle2560"
assert driver_info["CFBundleIdentifier"] == "org.iiyume.qle2560.driver"
for info in (app_info, driver_info):
    assert info["CFBundleName"] == "QLE2560 Driver"
    assert info["CFBundleDisplayName"] == "QLE2560 Driver"
    assert info["CFBundleShortVersionString"] == "0.19.0"
    assert info["CFBundleVersion"] == "19"
assert driver_info["CFBundlePackageType"] == "DEXT"
assert dext.name == driver_info["CFBundleIdentifier"] + ".dext"
personality = driver_info["IOKitPersonalities"]["QLE2560Driver"]
assert personality["IOUserServerName"] == driver_info["CFBundleIdentifier"]
assert personality["IOProviderClass"] == "IOPCIDevice"
assert personality["IOUserClass"] == "QLE2560Driver"
assert personality["IOClass"] == "IOUserSCSIParallelInterfaceController"
assert personality["CFBundleIdentifierKernel"] == "com.apple.iokit.IOSCSIParallelFamily"
assert personality["IOPCITunnelCompatible"] is True
assert personality["QLE2560ControlVersion"] == 2
assert personality["IOPCIPrimaryMatch"] == "0x25321077"
assert personality["IOPCISecondaryMatch"] == "0x015c1077"
assert len(list(app.rglob("*.dext"))) == 1, "Duplicate or misplaced extension"
entitlements = read(root / "driver/QLE2560Driver.entitlements")
pci = entitlements["com.apple.developer.driverkit.transport.pci"]
assert pci == [
    {key: personality[key] for key in ("IOPCIPrimaryMatch", "IOPCISecondaryMatch")}
]
development = read(root / "driver/QLE2560DriverDevelopment.entitlements")
assert development["com.apple.developer.driverkit.family.scsicontroller"] is True
assert entitlements["com.apple.developer.driverkit.family.scsicontroller"] is True
assert development["com.apple.developer.driverkit.transport.pci"] == [
    {"IOPCIPrimaryMatch": "0xFFFFFFFF&0x00000000"}
]
for binary in (
    app / "Contents/MacOS" / app_info["CFBundleExecutable"],
    dext / driver_info["CFBundleExecutable"],
):
    subprocess.run(["xcrun", "lipo", str(binary), "-verify_arch", "arm64"], check=True)
print(
    "PASS: ARM64 binaries, embedded extension location, bundle IDs, PCI matching, entitlements."
)
