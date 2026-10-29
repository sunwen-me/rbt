# rbt v0.6 RPC 交接

日期：2026-10-15。工作目录：`/home/sw/rbt`。

## 给接手 AI 的第一条指令

请继续这个仓库的 **v0.6 Typed Local RPC** 工作。先完整阅读原始规格、
`AGENTS.md` 和本交接，再检查当前文件与测试日志。不要重做已经完成的实现，
不要把实验测试通过当作正式版本验收通过，也不要清理或重置工作树。

原始规格共 4470 行，包含 230 节要求和 150 项验收清单：

`/home/sw/.codex/attachments/6f6bb2f6-8cf2-4da4-a47d-b5b6329d3b26/pasted-text-1.txt`

用户最后要求修复响应串线问题，随后要求写交接，由其他 AI 接手。
平台曾多次显示“无法显示此内容”，用户对此很困扰。这个界面提示不能用来
判断本地命令是否运行；应检查具体进程、句柄、退出码和落盘日志。

## 当前最重要的结论

**未修改的 iceoryx2 0.9.3 存在已独立复现的原生响应关联缺陷。**
原始规格第 229 节要求此类缺陷触发 hard stop；正式 v0.6 尚未验收。

**隔离目录中的原生修复实验已有进展：29 项测试通过。**

- 原生专项回归 6 项，包括 100 次客户端槽位复用。
- 直接复用仓库现有 Rust fixed RPC 测试 15 项。
- 现有 Rust dynamic RPC 测试 4 项，包含 PointCloud。
- 现有 Rust stress 测试 4 项，包含 1000 次 timeout 和 1000 次 cancel。

这些结果来自最近完成的实际命令，退出码 0：

`/home/sw/rbt/target/rpc-blocker-evidence/patched-rbt-regression.log`

**正式 Cargo 依赖、已安装的 C++ 库和 Python 扩展还没有切换到实验补丁。**
因此主工作树的新增串线回归测试对正式依赖仍应失败。不要删除、忽略或弱化它。
本聊天的 goal 工具最近返回状态为 `blocked`，未标记完成。

## 串线问题和原生修复

### 已复现的行为

1. 旧客户端发送自己的第一个请求；native request ID 和 channel 都从 0 开始。
2. 服务端保留旧 `ActiveRequest` 或已借出的旧响应。
3. 旧客户端丢弃 pending 并退出。
4. 新客户端连接同一个物理 server，复用 native 连接槽位，自己的请求 ID 也从 0 开始。
5. 服务端接收新请求，刷新槽位指向新客户端。
6. 旧响应通过缓存的数字 `connection_id` 发到该槽位。
7. 新 pending 只按 native request ID 过滤，接受旧响应。

这在直接调用 native iceoryx2、没有 rbt wrapper、sidecar、Composer 或指标层的
最小测试中也能复现。不是 rbt 的序列号测试数据造成的关联方案。

正式仓库中的复现：

`/home/sw/rbt/crates/rbt-iox2/tests/rpc_resources.rs`

- `stale_server_request_cannot_satisfy_reopened_clients_first_pending_call`
- `native_stale_response_cannot_cross_client_connection_reuse`

独立失败日志：

- `target/rpc-blocker-evidence/stale-client-reopen-root.log`
- `target/rpc-blocker-evidence/native-stale-client-reopen-root.log`
- `target/rpc-blocker-evidence/unpatched.log`

原生测试误收到 `(0xaaaa, 0xdead_beef)`；wrapper 预期新响应
`(0xbbbb, 0xcafe_babe)`，实际收到旧值。另一个问题是 native request ID
原本会按 `2^62` 取模，不能只靠 request ID 过滤就宣称全回绕安全。

### 精确上游和实验目录

未修改审计源：`/home/sw/rbt/target/iceoryx2-v0.9.3`

基准 commit：`6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4`。

实验修改源：`/home/sw/rbt/target/iceoryx2-rpc-correlation-fix`。

可保留、可搬走的补丁：

`/home/sw/rbt/experiments/rpc-native-correlation/iceoryx2-0.9.3-correlation.patch`

补丁 SHA256：

`bd615274932e308cd3da0c96575b10599f425d791edb18985466f947453c1ac9`

补丁包括：

- `iceoryx2/src/active_request.rs`：用复制保留的 `origin()` 身份，在原生
  shared-state 锁内重新寻找原客户端连接；保护 `finish/Drop`、`is_connected`
  和 `has_disconnect_hint`，防止旧对象操作新客户端通道。
- `iceoryx2/src/response_mut.rs`：响应保存原 `UniqueClientId` 和 request ID，
  在刷新连接后、同一把原生锁内验证身份和通道状态；旧客户端或请求失效时不投递。
- `iceoryx2/src/port/client.rs`：request ID 分配不再回绕，耗尽时返回现有
  `LoanError::InternalFailure`；在分配 chunk / 弹出 channel 前保留 ID，避免失败泄漏。
  新增了边界单测，但该上游内部单测尚未单独运行。
- `iceoryx2/Cargo.toml`：增加 `workspace = ".."`，解决实验同时依赖 rbt
  workspace 和嵌套原生 workspace 时 Cargo 错认 workspace 的问题。

没有修改用户 payload / wire header，没有新序列化、重试或自定义传输协议。
旧响应丢弃在实验中保留 native 的成功 no-op 语义，所以 `send() == Ok(())`
不能当作投递确认。

注意：native `ResponseMut` 的内部结构增加了字段。**C++/FFI opaque storage、
库与 headers 一致性还没有验证。** 正式采用时必须重建并验证各语言绑定，
不能只替换 Rust crate 后继续使用旧 native C++ 静态库。

独立实验说明：

`/home/sw/rbt/experiments/rpc-native-correlation/README.md`

该实验 Cargo.toml 直接引用 rbt 现有测试源作为 test targets，未复制或改弱断言。
实验自己的 Cargo.lock 已解析到本地补丁和相应 path dependencies；主 Cargo.lock
没有为这次实验改成 patch 来源。

## 接手后的最短有效下一步

先阅读原生补丁的实际 diff，验证边界单测、wrapper 两个串线测试和复用场景。
这能在正式依赖方案决策前继续收集证据。

环境中通常没有系统 `cc`。先设置：

```bash
cd /home/sw/rbt
export PATH="$PWD/target/phase4-toolchain/bin:$PATH"
export CC="$PWD/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$PWD/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-g++"
export AR="$PWD/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="$CC"
```

最近成功的 29 项测试命令：

```bash
CARGO_TARGET_DIR="$PWD/target/rpc-native-correlation-patched" \
cargo test --manifest-path experiments/rpc-native-correlation/Cargo.toml \
  --config "patch.crates-io.iceoryx2.path=\"$PWD/target/iceoryx2-rpc-correlation-fix/iceoryx2\"" \
  --offline --test native_correlation --test rbt-fixed \
  --test rbt-dynamic --test rbt-stress -- --test-threads=1
```

下一条尚未执行、值得先跑的命令：

```bash
CARGO_TARGET_DIR="$PWD/target/rpc-native-correlation-patched" \
cargo test --manifest-path experiments/rpc-native-correlation/Cargo.toml \
  --config "patch.crates-io.iceoryx2.path=\"$PWD/target/iceoryx2-rpc-correlation-fix/iceoryx2\"" \
  --offline --test rbt-resources stale_ -- --nocapture --test-threads=1
```

`rbt-resources` 还含 request loan exhaustion/recovery、in-flight capacity/recovery
和一个 100000-call 测试；这些新增测试并未全部跑完，不要宣称其通过。

正式集成前需要确定：如何维护/分发这个 0.9.3 原生修复、是否符合用户最初
exact 上游约束，以及 Rust/C++/Python 都使用相同修复来源的构建路径。
保持基准源码和补丁差异可审计；不能把 patched 0.9.3 冒称为未修改的上游。
若需要偏离原始规格，明确向用户说明边界。

## v0.6 已实现内容

### 核心与生命周期

- Rust fixed/fixed、fixed/slice、slice/fixed、slice/slice，native loan/borrow。
- Q/R 双侧 ABI preflight；native correlation，advisory RPC sidecars。
- WaitSet，初始 probe、100 ms safety probe；terminal CAS 处理 response/deadline/cancel/expiry。
- Bounded resources、pressure metrics、CLI graph/why/capability/rpc info。
- activation-owned endpoint teardown，provider replacement 与 100-cycle 生命周期测试。
- SIGKILL server/client 故障测试、16 clients、乱序响应、1000 timeout/cancel。

### Python

主要文件：`crates/rbt-python/src/rpc.rs`、`crates/rbt-python/src/lib.rs`、
`python/rbt/__init__.py`、`python/rbt/__init__.pyi`。

- fixed sync/async client/server。
- 每个 endpoint 一个 native bridge，多个调用共用；没有 thread per call。
- 动态 Image 三种组合，element_count、头部、memoryview/NumPy retained lease。
- N=0/1/100/100000、1 MiB 请求 / 20 MiB 响应、派生视图、跨线程 final drop 测试。
- 新增 `response_guards_current/high_water` 指标，Rust check/clippy 通过；
  对应 Python 新断言尚未重建扩展验证。
- 当前验证解释器：`target/python-verify-venv/bin/python`（CPython 3.12）。
  不要把旧 CPython 3.13 或 debug extension 拷回当前包。

### C++

主要文件：`sdk/cpp/include/rbt/rpc.hpp`、`sdk/cpp/src/runtime.cpp`、
`sdk/cpp/tests/rpc_tests.cpp`。

- native C++ payload path，move-only request/response RAII。
- 最近改动把 RpcClient 也改为 move-only，修复复制对象析构导致共享 endpoint 失效的问题。
- 允许 N=0 request；新增 `loan_response_slice(length)`，保留原 `loan_response()`。
- response guard current/high-water 指标及 move/drop 测试。
- 最近这些改动的 targeted `rpc_tests` 和 `runtime_tests` 通过。
- 它们仍链接未修复的原生库；原生补丁的 C++/FFI 重建尚未完成。

### Interop、soak、wheel、example

- `scripts/test_rpc_interop.sh`：早先 8 方向通过（6 fixed + 2 dynamic）。
- 最近加了 Rust/C++ 双向 burst，16 outstanding / 4096 calls；脚本现在声称10方向，
  **新增 burst 尚未运行**。文件在 Rust interop crate、C++ interop tool、Python probe。
- `scripts/soak_rpc.sh`：默认300秒、10次Composer restart、周期provider replacement、
  joint outage、resource与fresh metric检查。旧15秒smoke通过，最近增强后的完整脚本未运行。
- `python/tests/rpc_soak_probe.py`：2 cycles通过，正确回收registrations/tasks/threads。
- `scripts/verify_python.sh` 和 `python/tests/wheel_rpc_probe.py`：新增干净wheel环境
  的 async/deadline/cancel/reuse/dynamic/Rust互通；尚未完成当前wheel验收。
- 外部 installed Rust/C++ consumers 已含 fixed RPC 实际调用，正式安装门禁待跑。
- `examples/08-rpc`：一条命令启动；最近新增 `RBT_RPC_DEMO_DELAY_MS=1000`
  可演示250ms deadline及后续不同sequence调用，targeted build/smoke已通过。

## 验证状态与历史证据

最近中止的 full run：

`target/rpc-verify-logs/rpc-full-20261015T061829Z-24776`

中止前有：Rust fixed15/dynamic4/stress4/fault4、Python focused26、
CLI1、hash1、workspace tests/clippy；C++ Debug/Release/ASan/UBSan 各7项通过。
**这些是后续修改前的部分历史证据，不能当当前完整验收通过。**

完整 verifier 在发现原生阻塞后被显式终止。最后的进程检查没有运行中的
verify_rpc/cargo test/RPC component/soak 工作进程。所有子代理已完成或停止。

`scripts/verify_rpc.sh` 曾有 `--exact` 匹配0项的问题，已修：

- CLI：`tests::rpc_info_reports_typed_metrics_without_fabricating_unavailable_zeroes`
- hash：`tests::built_in_layout_hashes_are_fixed`
- lifecycle100：`component::tests::rpc_provider_replacement_stress_keeps_prepared_client_and_rotates_lifecycle`

历史 full run 的单独 lifecycle100 命令仍用了旧 filter，实际0项，不能算通过。
workspace中该测试曾运行。接手应以当前精确命令重新验证。

`verify_rpc.sh` 还没有把新增 `rpc_resources` 串线测试加到最早quick门禁；
目前 workspace test 会自然包含它们。集成修复后应确保门禁显式保留这些回归。

Python maturin 注意：环境常同时有 `CONDA_PREFIX` 和 `VIRTUAL_ENV`；
build时显式 `env -u CONDA_PREFIX VIRTUAL_ENV=... PYO3_PYTHON=...`。
`verify_rpc.sh` 已做此修正。历史Python publisher压力测试在debug扩展下存在
时序敏感性，正式门禁采用release构建，不要无证据归因到RPC。

## 冻结约束与版本

- Workspace、Python、C++、CLI、packaging当前开发版本已改为0.6.0；不是release accepted。
- IDL1；descriptor1；control2；snapshot4（新增rpc压力快照）；MCAP1。
- 正式 iceoryx2仍精确0.9.3；网络工作仍暂停。
- production unsafe最后完整审计49→49；最终当前工作树仍需复查。
- Image：`943fa4008cf0f55f89a281b051c69a6f`
- PointCloud：`d9da2bb29bb2cf217c40f1a56d1fb755`
- ControlCommand：`9381cf15842a48ac0964111f0f45ef72`
- Frame：`5d2c102a797d5b0ab64b4db8844297c0`
- Detection：`d8e60d2ca65e6b5b74f0981d34884a4d`
- 不增加用户payload元数据、自动重试、exactly-once承诺、自定义传输、网络RPC、
  generic JSON RPC/reflection、State或schema evolution；MCAP仍仅Streams。
- timeout/cancel不证明服务端未执行。provider availability属于Requirement生命周期。

## 报告和剩余工作

`docs/rpc-report.md` 已创建，包括blocked结论、原生复现、架构/ownership/race、
资源与错误语义、150项清单；清单按未修复正式依赖与当时证据填写。
`docs/rpc-iox2-notes.md` 原来错误的CONTINUE/回绕安全结论已更正。
实验通过不能让这些正式状态自动变绿；补丁来源正式确定并全量验证后再更新。

正式仍缺：补丁集成和审计、原生counter边界测试、wrapper资源全部测试、
C++/FFI修复版本、fresh Python extension/wheel、10方向interop、100-cycle精确门禁、
benchmark矩阵、300秒soak、各inherited全量门禁、external installed consumers、
最终hash/unsafe/version/process审计与逐项acceptance。

原始要求包含：

```bash
cargo test --workspace --locked
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
./scripts/verify_rpc.sh --quick
./scripts/verify_rpc.sh --full
```

full还编排DX、pressure、WaitSet、MCAP、Python/asyncio、v0.1 runtime、
interop、packaging等门禁，不能用29项实验回归代替。

## 工作树注意事项

当前仓库几乎整个源码树都是untracked，没有可靠tracked基线，`git diff`不会
展示这些文件的所有工作。**不要执行git clean、reset --hard或覆盖还原。**
用实际文件、日志、准确测试命令审阅；实验上游clone有正常git基线可看diff。

`AGENTS.md` 当前为gpt-6.1-sol/xhigh主决策、gpt-6-luna/max执行代理。
接手主代理负责架构、实际变更审阅与最终验收。当前没有未完成的代理任务。
