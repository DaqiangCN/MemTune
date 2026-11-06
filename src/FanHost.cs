/*
 * FanBridge.exe - 风扇控制常驻宿主
 *
 * 为什么要单独一个进程：
 *   LibreHardwareMonitor 的 LpcIO 只能被一个持有者打开（走 PawnIO 端口 I/O），
 *   而且初始化要几百毫秒。所以让它常驻，MemTune 用命名管道发指令。
 *
 * 协议（UTF-8，一行一条，回包也是一行）：
 *   SET <0-100>   设置所有风扇百分比   -> OK <n>   （n = 成功通道数）
 *   DEFAULT       交还 BIOS 自动控制   -> OK
 *   FANS          读当前转速 RPM       -> OK <f1,f2,...>
 *   TEMPS         读当前温度 °C        -> OK <t1,t2,...>
 *   QUIT          退出                 -> BYE
 *
 * 必须管理员权限运行：端口 I/O 无权限时 DetectIT87 直接返回 false。
 */
using System;
using System.Globalization;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Threading;

public static class FanHost
{
    private const string PIPE_NAME = "MemTuneFanCtl";
    private const string LHM_DLL   = "LibreHardwareMonitorLib.dll";
    private const string BRIDGE    = "FanBridge.dll";

    private static string _logPath;

    private static void Log(string msg)
    {
        try
        {
            if (_logPath == null) return;
            File.AppendAllText(_logPath,
                DateTime.Now.ToString("HH:mm:ss") + " " + msg + Environment.NewLine,
                Encoding.UTF8);
        }
        catch { }
    }

    public static int Main(string[] args)
    {
        string root = AppDomain.CurrentDomain.BaseDirectory;
        _logPath = Path.Combine(root, "fanbridge.log");

        bool once = false;
        foreach (string a in args)
            if (a == "--test" || a == "/test") once = true;

        /* 加载桥接 DLL 并初始化 */
        int channels = 0;
        string err = "";
        try
        {
            System.Reflection.Assembly.LoadFrom(Path.Combine(root, BRIDGE));
            t = Type.GetType("FanBridge, FanBridge");
            if (t == null)
            {
                /* 没写命名空间时类型在全局 */
                foreach (System.Reflection.Assembly asm in AppDomain.CurrentDomain.GetAssemblies())
                {
                    if (asm.GetName().Name != "FanBridge") continue;
                    t = asm.GetType("FanBridge");
                    if (t != null) break;
                }
            }
            if (t == null) { Log("FanBridge type not found"); return 2; }

            object r = t.GetMethod("Init").Invoke(null,
                new object[] { Path.Combine(root, LHM_DLL) });
            channels = Convert.ToInt32(r);
            err = (string)t.GetProperty("LastError").GetValue(null, null);
        }
        catch (Exception e)
        {
            Log("init exception: " + (e.InnerException != null ? e.InnerException.Message : e.Message));
            return 2;
        }

        if (channels <= 0)
        {
            Log("init failed: " + err);
            Console.Error.WriteLine("FANBRIDGE_INIT_FAIL " + err);
            return 3;
        }

        Log("online, " + channels + " channels");

        if (once)
        {
            Console.WriteLine("OK " + channels);
            return 0;
        }

        /* 主循环：一条管道对应一次连接，MemTune 会复用同一条 */
        while (true)
        {
            try
            {
                using (NamedPipeServerStream srv = new NamedPipeServerStream(
                           PIPE_NAME, PipeDirection.InOut, 1,
                           PipeTransmissionMode.Byte, PipeOptions.None))
                {
                    srv.WaitForConnection();
                    Log("client connected");

                    using (StreamReader rd = new StreamReader(srv, Encoding.UTF8, false, 256, true))
                    using (StreamWriter wr = new StreamWriter(srv, new UTF8Encoding(false), 256, true))
                    {
                        wr.AutoFlush = true;
                        bool quit = false;

                        while (!quit)
                        {
                            string line = rd.ReadLine();
                            if (line == null) break;
                            line = line.Trim();
                            if (line.Length == 0) continue;

                            string reply;
                            try
                            {
                                reply = Handle(t, line);
                            }
                            catch (Exception e)
                            {
                                reply = "ERR " + (e.InnerException != null
                                                 ? e.InnerException.Message
                                                 : e.Message);
                            }
                            if (reply == "BYE") { quit = true; }
                            wr.WriteLine(reply);
                        }
                    }
                    Log("client disconnected");
                }
            }
            catch (Exception e)
            {
                Log("pipe loop error: " + e.Message);
                Thread.Sleep(500);
            }
        }
    }

    private static Type t = null;

    private static string Handle(Type type, string line)
    {
        string[] p = line.Split(' ');
        string cmd = p[0].ToUpperInvariant();

        switch (cmd)
        {
            case "SET":
            {
                int pct;
                if (p.Length < 2 || !int.TryParse(p[1], out pct)) return "ERR bad value";
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                type.GetMethod("Update").Invoke(null, null);
                object n = type.GetMethod("SetPercent").Invoke(null, new object[] { pct });
                Log("set " + pct + "% -> " + n);
                return "OK " + n;
            }
            case "DEFAULT":
                type.GetMethod("SetDefault").Invoke(null, null);
                Log("default restored");
                return "OK";
            case "FANS":
                type.GetMethod("Update").Invoke(null, null);
                return "OK " + (string)type.GetMethod("ReadFans").Invoke(null, null);
            case "TEMPS":
                type.GetMethod("Update").Invoke(null, null);
                return "OK " + (string)type.GetMethod("ReadTemps").Invoke(null, null);
            case "QUIT":
                try { type.GetMethod("SetDefault").Invoke(null, null); } catch { }
                return "BYE";
            default:
                return "ERR unknown " + cmd;
        }
    }
}
