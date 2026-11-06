using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Threading;
using System.Windows.Forms;

// MemTune 单文件安装器：把整套文件（含依赖与 PawnIO 安装器）内嵌在 exe 里，
// 运行时解到临时目录，再调用包内的 Install.ps1 完成实际安装。
public static class MemTuneInstaller
{
    private const string ResourceName = "payload.zip";
    private const string PayloadRoot = "MemTune-Setup";

    public static int Main(string[] args)
    {
        bool silent = HasArg(args, "/silent") || HasArg(args, "-silent");
        bool uninstall = HasArg(args, "/uninstall") || HasArg(args, "-uninstall");
        string dirArg = GetValue(args, "/dir=") ?? GetValue(args, "-dir=");
        string tempDir = Path.Combine(Path.GetTempPath(), "MemTuneSetup_" + Guid.NewGuid().ToString("N"));

        StatusForm form = null;
        if (!silent)
        {
            form = new StatusForm(uninstall ? "正在卸载 MemTune，请稍候…" : "正在安装 MemTune，请稍候…");
            form.Show();
            form.Refresh();
            Application.DoEvents();
        }

        int exitCode = 1;
        string message = "";
        try
        {
            Directory.CreateDirectory(tempDir);
            if (!uninstall)
            {
                ExtractPayload(tempDir);
            }
            exitCode = RunScript(tempDir, uninstall, dirArg, silent, out message);
        }
        catch (Exception ex)
        {
            exitCode = 1;
            message = "安装器出错：" + ex.Message;
        }
        finally
        {
            if (form != null) { form.Close(); }
            try { Directory.Delete(tempDir, true); } catch { }
        }

        if (!silent)
        {
            if (exitCode == 0 && string.IsNullOrEmpty(message))
            {
                message = uninstall
                    ? "MemTune 已卸载完成。"
                    : "MemTune 安装完成。\n\n从桌面或开始菜单启动即可（首次会请求管理员权限，这是风扇读写主板端口所必需的）。";
            }
            else if (exitCode != 0 && string.IsNullOrEmpty(message))
            {
                message = "安装未成功（脚本退出码 " + exitCode + "）。请把安装目录下的日志发给我。";
            }
            MessageBox.Show(message, "MemTune", MessageBoxButtons.OK,
                            exitCode == 0 ? MessageBoxIcon.Information : MessageBoxIcon.Warning);
        }
        else if (!string.IsNullOrEmpty(message))
        {
            Console.WriteLine(message);
        }
        return exitCode;
    }

    // 把内嵌的 zip 解到 tempDir
    private static void ExtractPayload(string tempDir)
    {
        Assembly asm = Assembly.GetExecutingAssembly();
        using (Stream s = asm.GetManifestResourceStream(ResourceName))
        {
            if (s == null) throw new Exception("安装包损坏：找不到内嵌文件");
            using (ZipArchive zip = new ZipArchive(s, ZipArchiveMode.Read))
            {
                foreach (ZipArchiveEntry e in zip.Entries)
                {
                    if (e.FullName.EndsWith("/")) continue;
                    string target = Path.Combine(tempDir, e.FullName.Replace('/', '\\'));
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    using (Stream input = e.Open())
                    using (FileStream output = new FileStream(target, FileMode.Create, FileAccess.Write))
                    {
                        input.CopyTo(output);
                    }
                }
            }
        }
    }

    // 调用包内脚本
    private static int RunScript(string tempDir, bool uninstall, string dirArg, bool silent, out string message)
    {
        message = null;
        string script;
        string pwshArgs;

        if (uninstall)
        {
            string installDir = string.IsNullOrEmpty(dirArg)
                ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "MemTune")
                : dirArg;
            script = Path.Combine(installDir, "Uninstall.ps1");
            if (!File.Exists(script))
            {
                message = "没找到卸载脚本：\n" + script;
                return 1;
            }
            pwshArgs = "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" -Silent";
        }
        else
        {
            script = Path.Combine(Path.Combine(tempDir, PayloadRoot), "Install.ps1");
            if (!File.Exists(script))
            {
                message = "安装包不完整：缺少 Install.ps1";
                return 1;
            }
            pwshArgs = "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\"";
            if (!string.IsNullOrEmpty(dirArg)) pwshArgs += " -InstallDir \"" + dirArg + "\"";
        }

        string pwsh = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
                                   @"WindowsPowerShell\v1.0\powershell.exe");

        ProcessStartInfo psi = new ProcessStartInfo(pwsh, pwshArgs);
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        psi.WorkingDirectory = tempDir;

        using (Process p = Process.Start(psi))
        {
            string stdout = p.StandardOutput.ReadToEnd();
            string stderr = p.StandardError.ReadToEnd();
            p.WaitForExit();

            if (p.ExitCode != 0)
            {
                string detail = (stderr + "\n" + stdout).Trim();
                if (detail.Length > 1200) detail = detail.Substring(detail.Length - 1200);
                message = "脚本执行失败（退出码 " + p.ExitCode + "）：\n\n" + detail;
            }
            else
            {
                // 成功后把自检结论带出来（Install.ps1 会打印 12 通道之类的信息）
                foreach (string line in stdout.Split('\n'))
                {
                    if (line.Contains("风扇可用") || line.Contains("PawnIO") || line.Contains("自检未通过"))
                    {
                        message = message == null ? "" : message;
                        message += line.Trim() + "\n";
                    }
                }
                if (message != null && message.Length > 0 && p.ExitCode == 0)
                {
                    message = message.Trim() + "\n\n" + (uninstall ? "" : "安装完成。");
                }
                else
                {
                    message = null;
                }
            }
            return p.ExitCode;
        }
    }

    private static bool HasArg(string[] args, string key)
    {
        foreach (string a in args)
            if (string.Equals(a, key, StringComparison.OrdinalIgnoreCase)) return true;
        return false;
    }

    private static string GetValue(string[] args, string prefix)
    {
        foreach (string a in args)
        {
            if (a.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            {
                string v = a.Substring(prefix.Length).Trim('"');
                return v;
            }
        }
        return null;
    }
}

// 一个极简的进度窗口（不阻塞安装线程）
internal sealed class StatusForm : Form
{
    public StatusForm(string text)
    {
        Text = "MemTune";
        Width = 460;
        Height = 150;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        StartPosition = FormStartPosition.CenterScreen;
        ControlBox = false;
        MaximizeBox = false;
        MinimizeBox = false;
        TopMost = true;

        Label label = new Label();
        label.Text = text;
        label.AutoSize = false;
        label.SetBounds(24, 20, 400, 40);
        label.Font = new System.Drawing.Font("Microsoft YaHei UI", 10f);
        Controls.Add(label);

        ProgressBar bar = new ProgressBar();
        bar.Style = ProgressBarStyle.Marquee;
        bar.MarqueeAnimationSpeed = 30;
        bar.SetBounds(24, 70, 400, 22);
        Controls.Add(bar);
    }
}
