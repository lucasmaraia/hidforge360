$code = @"
using System;
using System.Runtime.InteropServices;

public class Hid {
  [StructLayout(LayoutKind.Sequential)]
  public struct SP_DEVICE_INTERFACE_DATA {
    public int cbSize; public Guid InterfaceClassGuid; public int Flags; public IntPtr Reserved;
  }
  [StructLayout(LayoutKind.Sequential)]
  public struct HIDD_ATTRIBUTES {
    public int Size; public ushort VendorID; public ushort ProductID; public ushort VersionNumber;
  }
  [StructLayout(LayoutKind.Sequential)]
  public struct HIDP_CAPS {
    public ushort Usage; public ushort UsagePage;
    public ushort InputReportByteLength; public ushort OutputReportByteLength; public ushort FeatureReportByteLength;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst=17)] public ushort[] Reserved;
    public ushort NumberLinkCollectionNodes;
    public ushort NumberInputButtonCaps; public ushort NumberInputValueCaps; public ushort NumberInputDataIndices;
    public ushort NumberOutputButtonCaps; public ushort NumberOutputValueCaps; public ushort NumberOutputDataIndices;
    public ushort NumberFeatureButtonCaps; public ushort NumberFeatureValueCaps; public ushort NumberFeatureDataIndices;
  }

  [DllImport("setupapi.dll", CharSet=CharSet.Auto)]
  public static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr enumerator, IntPtr hwnd, int flags);
  [DllImport("setupapi.dll", CharSet=CharSet.Auto)]
  public static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr devInfo, ref Guid g, int index, ref SP_DEVICE_INTERFACE_DATA data);
  [DllImport("setupapi.dll", CharSet=CharSet.Auto)]
  public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr set, ref SP_DEVICE_INTERFACE_DATA data, IntPtr detail, int detailSize, ref int required, IntPtr devInfo);
  [DllImport("setupapi.dll")]
  public static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

  [DllImport("kernel32.dll", CharSet=CharSet.Auto, SetLastError=true)]
  public static extern IntPtr CreateFile(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool WriteFile(IntPtr h, byte[] buf, int len, out int written, IntPtr ov);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool ReadFile(IntPtr h, byte[] buf, int len, out int read, IntPtr ov);
  [DllImport("kernel32.dll")]
  public static extern bool CloseHandle(IntPtr h);

  [DllImport("hid.dll")] public static extern bool HidD_GetAttributes(IntPtr h, ref HIDD_ATTRIBUTES a);
  [DllImport("hid.dll")] public static extern bool HidD_GetPreparsedData(IntPtr h, out IntPtr data);
  [DllImport("hid.dll")] public static extern bool HidD_FreePreparsedData(IntPtr data);
  [DllImport("hid.dll")] public static extern int  HidP_GetCaps(IntPtr data, ref HIDP_CAPS caps);
  [DllImport("hid.dll", CharSet=CharSet.Unicode)] public static extern bool HidD_GetProductString(IntPtr h, byte[] buf, int len);
  [DllImport("hid.dll", CharSet=CharSet.Unicode)] public static extern bool HidD_GetManufacturerString(IntPtr h, byte[] buf, int len);
  [DllImport("hid.dll")] public static extern bool HidD_SetOutputReport(IntPtr h, byte[] buf, int len);
  [DllImport("hid.dll")] public static extern bool HidD_GetFeature(IntPtr h, byte[] buf, int len);
  [DllImport("hid.dll")] public static extern bool HidD_SetFeature(IntPtr h, byte[] buf, int len);

  public static Guid HidGuid = new Guid("4D1E55B2-F16F-11CF-88CB-001111000030");

  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool DeviceIoControl(IntPtr h, uint code, byte[] inBuf, int inLen, byte[] outBuf, int outLen, out int ret, IntPtr ov);

  // Asks the hub for a descriptor of the device on one of its ports
  // (IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION). type 1 = device, 2 = configuration.
  public static byte[] UsbDescriptor(string hubPath, int port, int type, int length) {
    IntPtr h = CreateFile(hubPath, 0x40000000u, 2, IntPtr.Zero, 3, 0, IntPtr.Zero);
    if (h == IntPtr.Zero || h == new IntPtr(-1)) return null;
    var buf = new byte[12 + length];
    BitConverter.GetBytes(port).CopyTo(buf, 0);
    buf[4] = 0x80; buf[5] = 0x06;
    BitConverter.GetBytes((ushort)(type << 8)).CopyTo(buf, 6);
    BitConverter.GetBytes((ushort)length).CopyTo(buf, 10);
    int ret;
    bool ok = DeviceIoControl(h, 0x220410, buf, buf.Length, buf, buf.Length, out ret, IntPtr.Zero);
    CloseHandle(h);
    if (!ok || ret <= 12) return null;
    var d = new byte[ret - 12];
    Array.Copy(buf, 12, d, 0, d.Length);
    return d;
  }

  public static string[] Paths() {
    var g = HidGuid;
    IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, 0x12);
    var list = new System.Collections.Generic.List<string>();
    var d = new SP_DEVICE_INTERFACE_DATA();
    d.cbSize = Marshal.SizeOf(d);
    for (int i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref d); i++) {
      int need = 0;
      SetupDiGetDeviceInterfaceDetail(set, ref d, IntPtr.Zero, 0, ref need, IntPtr.Zero);
      IntPtr buf = Marshal.AllocHGlobal(need);
      Marshal.WriteInt32(buf, IntPtr.Size == 8 ? 8 : 6);
      if (SetupDiGetDeviceInterfaceDetail(set, ref d, buf, need, ref need, IntPtr.Zero))
        list.Add(Marshal.PtrToStringAuto(new IntPtr(buf.ToInt64() + 4)));
      Marshal.FreeHGlobal(buf);
      d.cbSize = Marshal.SizeOf(d);
    }
    SetupDiDestroyDeviceInfoList(set);
    return list.ToArray();
  }

  public static IntPtr Open(string path, bool write) {
    uint access = write ? 0xC0000000u : 0x80000000u;
    return CreateFile(path, access, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
  }

  public static string Str(IntPtr h, bool product) {
    var buf = new byte[256];
    bool ok = product ? HidD_GetProductString(h, buf, buf.Length) : HidD_GetManufacturerString(h, buf, buf.Length);
    if (!ok) return "";
    return System.Text.Encoding.Unicode.GetString(buf).TrimEnd('\0');
  }
}
"@

if (-not ("Hid" -as [type])) { Add-Type -TypeDefinition $code -Language CSharp }

function Get-SonyPad {
  foreach ($path in [Hid]::Paths()) {
    $h = [Hid]::Open($path, $false)
    if ($h -eq [IntPtr]::Zero -or $h -eq [IntPtr]-1) { continue }
    $a = New-Object Hid+HIDD_ATTRIBUTES
    $a.Size = [System.Runtime.InteropServices.Marshal]::SizeOf($a)
    $hit = $null
    if ([Hid]::HidD_GetAttributes($h, [ref]$a) -and $a.VendorID -eq 0x054C -and
        @(0x09cc, 0x05c4, 0x0ba0) -contains [int]$a.ProductID) {
      $caps = New-Object Hid+HIDP_CAPS
      $pre = [IntPtr]::Zero
      $out = 0; $in = 0; $feat = 0
      if ([Hid]::HidD_GetPreparsedData($h, [ref]$pre)) {
        [void][Hid]::HidP_GetCaps($pre, [ref]$caps)
        [void][Hid]::HidD_FreePreparsedData($pre)
        $in = $caps.InputReportByteLength; $out = $caps.OutputReportByteLength; $feat = $caps.FeatureReportByteLength
      }
      $hit = [pscustomobject]@{
        PID = ('{0:x4}' -f $a.ProductID); Produto = [Hid]::Str($h, $true)
        In = $in; Out = $out; Feature = $feat; Path = $path
      }
    }
    [void][Hid]::CloseHandle($h)
    if ($hit) { return $hit }
  }
  return $null
}

# Any HID collection by VID/PID; -Usage picks the collection (5 = gamepad).
function Get-HidPad([int]$Vid, [int]$ProductId, [int]$Usage = 5) {
  foreach ($path in [Hid]::Paths()) {
    $h = [Hid]::Open($path, $false)
    if ($h -eq [IntPtr]::Zero -or $h -eq [IntPtr]-1) { continue }
    $a = New-Object Hid+HIDD_ATTRIBUTES
    $a.Size = [System.Runtime.InteropServices.Marshal]::SizeOf($a)
    $hit = $null
    if ([Hid]::HidD_GetAttributes($h, [ref]$a) -and $a.VendorID -eq $Vid -and $a.ProductID -eq $ProductId) {
      $caps = New-Object Hid+HIDP_CAPS
      $pre = [IntPtr]::Zero
      if ([Hid]::HidD_GetPreparsedData($h, [ref]$pre)) {
        [void][Hid]::HidP_GetCaps($pre, [ref]$caps)
        [void][Hid]::HidD_FreePreparsedData($pre)
      }
      if ($caps.Usage -eq $Usage) {
        $hit = [pscustomobject]@{
          PID = ('{0:x4}' -f $a.ProductID); Versao = ('{0:x4}' -f $a.VersionNumber)
          Fabricante = [Hid]::Str($h, $false); Produto = [Hid]::Str($h, $true)
          UsagePage = $caps.UsagePage; Usage = $caps.Usage
          In = $caps.InputReportByteLength; Out = $caps.OutputReportByteLength
          Feature = $caps.FeatureReportByteLength; Path = $path
        }
      }
    }
    [void][Hid]::CloseHandle($h)
    if ($hit) { return $hit }
  }
  return $null
}
