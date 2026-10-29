# Git 工作流

`main` 保存当前整理后的框架源码。功能开发使用 `codex/<任务名>` 分支；
机器人功能包在 `/home/sw/rbt_robot_ws` 的独立仓库提交。

```bash
git status --short --branch
git switch -c codex/my-change
git diff
git add path/to/changed-file
git diff --cached
git commit -m "fix: describe the resulting behavior"
```

每次提交只包含同一项任务的源码和必要文档。按改动范围执行现有检查，真实记录结果。
提交前确认暂存区不包含构建产物、录包、现场地图、凭据或本地兼容链接。
框架发布打包的回归检查为 `python3 -m unittest discover -s tests/release -v`；
其他测试入口见 [开发指南](development.md)。

整理前基线保留在 `baseline/pre-split-20261029`。需要查看完整旧目录时，
在独立目录检出该标签，避免旧目录结构影响本机兼容链接：

```bash
git worktree add --detach ../rbt-before-split baseline/pre-split-20261029
```

远端配置通过 `git remote -v` 查看。当前没有 `origin`，本地提交尚未推送到 GitHub。
后续远端确定后，先核对仓库所有者和可见性，再设置地址和推送。
