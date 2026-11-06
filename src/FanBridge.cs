/*
 * FanBridge.cs - MemTune 的风扇直连桥
 *
 * 作用：在进程内托管 LibreHardwareMonitor，直接读写主板 SuperIO 风扇控制，
 *       完全取代原来的 fanctl.ps1 + fan_target.txt 文件通信方案。
 *
 * 为什么不能直接 new Computer().Open()：
 *   LHM 0.9.6 的主板型号表里没有 "Gigabyte B650M D2HP"，
 *   Model 会被识别成 Unknown，导致它不去检测 SuperIO，SubHardware 恒为 0。
 *   但芯片本身（ITE IT8689E）LHM 认识，只要手动调 LpcIO.DetectIT87(0x2E/0x4E)
 *   就能拿到 2 个 IT87XX 实例，每个 6 路风扇 + 6 路温度。
 *
 * 注意：端口 I/O 需要管理员权限，否则 DetectIT87 直接返回 false。
 */
using System;
using System.Collections.Generic;
using System.Reflection;

public static class FanBridge
{
    private const BindingFlags BF =
        BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;

    private static List<object> _superIOs = new List<object>();
    private static int _controlCount = 0;
    private static string _lastError = "";
    private static bool _inited = false;
    private static int _lastApplied = -1;

    public static string LastError { get { return _lastError; } }
    public static int SuperIOCount { get { return _superIOs.Count; } }
    public static int ControlCount { get { return _controlCount; } }

    /* IT87XX 用显式接口实现 ISuperIO.SetControl，类上 GetMethod 找不到，
       必须沿接口链找。 */
    private static MethodInfo FindSetControl(Type t)
    {
        /* IT87XX 的实现签名是 SetControl(int, byte?)，不是 float? */
        MethodInfo m = t.GetMethod("SetControl", new[] { typeof(int), typeof(byte?) });
        if (m != null) return m;

        m = t.GetMethod("SetControl", new[] { typeof(int), typeof(float?) });
        if (m != null) return m;

        foreach (Type it in t.GetInterfaces())
        {
            if (it.Name != "ISuperIO" && it.Name != "IControl") continue;
            m = it.GetMethod("SetControl", new[] { typeof(int), typeof(byte?) })
             ?? it.GetMethod("SetControl", new[] { typeof(int), typeof(float?) });
            if (m != null) return m;
        }

        /* 兜底：不限参数签名地找同名方法 */
        foreach (MethodInfo mi in t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic |
                                               BindingFlags.Instance))
        {
            if (mi.Name == "SetControl" && mi.GetParameters().Length == 2) return mi;
        }
        return null;
    }

    /* 初始化。成功返回风扇控制通道数，失败返回 0（详情看 LastError / LastError）。 */
    public static int Init(string dllPath)
    {
        if (_inited) return _controlCount;
        _inited = true;
        _superIOs.Clear();
        _controlCount = 0;
        _lastError = "";

        try
        {
            Assembly asm = Assembly.LoadFrom(dllPath);

            Type tSettings = asm.GetType("LibreHardwareMonitor.Hardware.Computer+Settings");
            Type tSmbios   = asm.GetType("LibreHardwareMonitor.Hardware.SMBios");
            Type tBoard    = asm.GetType("LibreHardwareMonitor.Hardware.Motherboard.Motherboard");
            Type tLpcIO    = asm.GetType("LibreHardwareMonitor.Hardware.Motherboard.Lpc.LpcIO");
            Type tLpcPort  = asm.GetType("LibreHardwareMonitor.Hardware.Motherboard.Lpc.LpcPort");
            if (tSettings == null || tSmbios == null || tBoard == null ||
                tLpcIO == null || tLpcPort == null)
            {
                _lastError = "LHM types missing";
                return 0;
            }

            object settings = Activator.CreateInstance(tSettings, true);
            object smbios   = Activator.CreateInstance(tSmbios, true);
            object board    = Activator.CreateInstance(tBoard, new object[] { smbios, settings });

            /* LpcIO 构造时只记下 board，真正的探测在 Detect* 里做 */
            object lpc = Activator.CreateInstance(tLpcIO, BF, null,
                                                  new object[] { board }, null);

            MethodInfo mDetect = tLpcIO.GetMethod("DetectIT87", BF);
            if (mDetect == null) { _lastError = "DetectIT87 missing"; return 0; }

            /* 标准 LPC 端口 0x2E/0x4E，一次就够（内部会扫到两颗 IT87XX） */
            object port = Activator.CreateInstance(tLpcPort, new object[]
                                                    { (ushort)0x2E, (ushort)0x4E });
            object ok = mDetect.Invoke(lpc, new object[] { port, board });
            if (!(ok is bool) || !(bool)ok)
            {
                _lastError = "DetectIT87 failed (need admin?)";
                return 0;
            }

            PropertyInfo pSuperIO = tLpcIO.GetProperty("SuperIO", BF);
            Array arr = pSuperIO.GetValue(lpc, null) as Array;
            if (arr == null || arr.Length == 0) { _lastError = "no SuperIO"; return 0; }

            foreach (object sio in arr)
            {
                _superIOs.Add(sio);
                PropertyInfo pCtrl = sio.GetType().GetProperty("Controls", BF);
                Array ctls = pCtrl.GetValue(sio, null) as Array;
                if (ctls != null) _controlCount += ctls.Length;
            }

            if (_controlCount == 0) { _lastError = "no controls"; return 0; }
            return _controlCount;
        }
        catch (Exception e)
        {
            _lastError = (e.InnerException != null ? e.InnerException.Message : e.Message);
            return 0;
        }
    }

    /* 刷新传感器缓存（LHM 要求每次读前 Update，否则数值不更新） */
    public static void Update()
    {
        foreach (object sio in _superIOs)
        {
            try
            {
                MethodInfo m = sio.GetType().GetMethod("Update", BF, null, Type.EmptyTypes, null);
                if (m != null) m.Invoke(sio, null);
            }
            catch { }
        }
    }

    /* 设置所有风扇为软件控制百分比（0-100）。返回成功的通道数。 */
    public static int SetPercent(int percent)
    {
        if (percent < 0)   percent = 0;
        if (percent > 100) percent = 100;

        int ok = 0;
        foreach (object sio in _superIOs)
        {
            try
            {
                PropertyInfo pCtrl = sio.GetType().GetProperty("Controls", BF);
                Array ctls = pCtrl.GetValue(sio, null) as Array;
                if (ctls == null) continue;
                MethodInfo m = FindSetControl(sio.GetType());
                if (m == null) { _lastError = "SetControl method not found on " + sio.GetType().Name; continue; }
                if (m == null) continue;
                for (int i = 0; i < ctls.Length; ++i)
                {
                    try
                    {
                        object boxed = Convert.ToByte(percent <= 100 ? percent * 255 / 100 : 255);
                        m.Invoke(sio, new object[] { i, boxed });
                        ok++;
                    }
                    catch (Exception e)
                    {
                        _lastError = "SetControl: " + (e.InnerException != null ? e.InnerException.Message : e.Message);
                    }
                }
            }
            catch { }
        }
        _lastApplied = percent;
        return ok;
    }

    /* 交还 BIOS 自动控制 */
    public static void SetDefault()
    {
        foreach (object sio in _superIOs)
        {
            try
            {
                PropertyInfo pCtrl = sio.GetType().GetProperty("Controls", BF);
                Array ctls = pCtrl.GetValue(sio, null) as Array;
                if (ctls == null) continue;
                MethodInfo m = FindSetControl(sio.GetType());
                if (m == null) { _lastError = "SetControl method not found on " + sio.GetType().Name; continue; }
                if (m == null) continue;
                for (int i = 0; i < ctls.Length; ++i)
                {
                    try
                    {
                        m.Invoke(sio, new object[] { i, null });
                    }
                    catch (Exception e)
                    {
                        _lastError = "SetControl(null): " + (e.InnerException != null ? e.InnerException.Message : e.Message);
                    }
                }
            }
            catch { }
        }
        _lastApplied = -1;
    }

    public static int LastApplied { get { return _lastApplied; } }

    /* 读所有风扇转速（RPM），返回逗号分隔，失败返回空串 */
    public static string ReadFans()
    {
        var sb = new System.Text.StringBuilder();
        try
        {
            foreach (object sio in _superIOs)
            {
                PropertyInfo p = sio.GetType().GetProperty("Fans", BF);
                Array fans = p.GetValue(sio, null) as Array;
                if (fans == null) continue;
                foreach (object f in fans)
                {
                    float? v = f as float?;
                    if (sb.Length > 0) sb.Append(',');
                    sb.Append(v.HasValue ? ((int)v.Value).ToString() : "-");
                }
            }
        }
        catch { }
        return sb.ToString();
    }

    /* 读所有温度（°C），返回逗号分隔 */
    public static string ReadTemps()
    {
        var sb = new System.Text.StringBuilder();
        try
        {
            foreach (object sio in _superIOs)
            {
                PropertyInfo p = sio.GetType().GetProperty("Temperatures", BF);
                Array ts = p.GetValue(sio, null) as Array;
                if (ts == null) continue;
                foreach (object t in ts)
                {
                    float? v = t as float?;
                    if (sb.Length > 0) sb.Append(',');
                    sb.Append(v.HasValue ? ((int)v.Value).ToString() : "-");
                }
            }
        }
        catch { }
        return sb.ToString();
    }
}
