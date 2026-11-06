# 第三方组件说明 / Third-Party Notices

本安装包分发以下第三方组件，均为原版未修改。各自的许可证以官方仓库为准，发布时请一并保留本文件。

## 1. LibreHardwareMonitorLib.dll

- 来源：https://github.com/LibreHardwareMonitor/LibreHardwareMonitor
- 版本：0.9.6.0
- 许可证：MPL-2.0（Mozilla Public License 2.0）
- 说明：本程序仅通过反射调用其 `LpcIO` / `IT87XX` 相关 API 读取 SuperIO 芯片，未修改其源码。
- 附带的依赖 DLL：`HidSharp.dll`、`RAMSPDToolkit-NDD.dll`、`System.Buffers.dll`、`System.Memory.dll`、`System.Numerics.Vectors.dll`、`System.Runtime.CompilerServices.Unsafe.dll`、`System.Threading.Tasks.Extensions.dll`、`System.Collections.Immutable.dll`、`System.Text.Json.dll`、`System.Text.Encodings.Web.dll`、`System.Security.AccessControl.dll`、`System.Security.Principal.Windows.dll`、`System.Threading.AccessControl.dll`、`Microsoft.Bcl.AsyncInterfaces.dll`、`Microsoft.Bcl.HashCode.dll`

## 2. PawnIO

- 来源：https://pawnio.eu/ ，发布页 https://github.com/namazso/PawnIO_setup
- 版本：2.2.0（`PawnIO_setup.exe`，签名者 namazso.eu）
- 分发条款：其安装器声明允许**原样再分发**（"This installer can be redistributed unmodified."），因此本包内保持原始文件不做任何改动。
- 说明：PawnIO 是内核态端口 I/O 驱动，本程序用它读写 SuperIO 端口（0x2E/0x4E）。它需要以管理员身份安装。

## 3. 其他

- .NET Framework 4.8：随 Windows 提供，不随本包分发。
