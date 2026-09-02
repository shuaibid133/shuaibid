# Git 使用速查（KazepOS 项目）

> 心理模型：**Git = 游戏存档**。`commit` = 存档点，存得越多越不怕改崩。

## 一、概念（就一个图）

```
工作区 ──git add──▶ 暂存区 ──git commit──▶ 仓库（历史存档）
(当前代码)         (选中待存)             (永久快照，每个都能回去)
```

- `HEAD` = 当前站在时间线的哪（最新存档）
- `git status` 永远先敲——告诉你"改了啥、存了没"

## 二、日常循环（90% 场景就这 4 条）

```bash
git status                        # ① 我改了啥？
git diff                          # ② 具体改了哪几行（提交前检查）
git add -A                        # ③ 全部选中，准备存
git commit -m "模块: 做了什么(为什么)"  # ④ 存档
git log --oneline                 # 回看存档列表（每行=一个存档，编号在前）
```

提交信息格式：`模块: 动作` 开头 + 说"为什么"。差信息（如 `update`）等于没写。

## 三、查看以前的代码（安全，只看不改）

```bash
git log --oneline                  # 存档列表 → 拿编号
git show 编号                      # 这个存档改了什么（diff）
git show 编号:App/Src/app.c        # 这个文件当时长啥样
git log --oneline -- App/Src/app.c # 某个文件自己的历史
```

## 四、回退（后悔药，改变代码要小心）

```bash
git checkout -- 文件.c             # 放弃未提交的改动（回到上次存档）★最常用
git checkout 编号 -- 文件.c        # 把某文件恢复到某存档的样子
git reset --hard HEAD~1            # 撤销最后一次提交（会丢改动，慎用）
git revert 编号                    # 生成反向提交（保留历史，安全）
```

## 五、两个坑

| 坑 | 现象 | 解决 |
|---|---|---|
| 编辑器卡住 | commit 弹出黑屏编辑器（`~` 波浪线） | 敲 `:q!` 回车退出，改用 `-m` |
| 一堆红色 | status 显示 M/?? | 正常！红色=改了没存，想存就 add+commit |

## 六、答辩演示（3 句词）

```
git log --oneline → "项目按功能/修复提交，每条对应一个验收点"
git show 编号     → "比如这次修复，当时改了这几处"
git status        → "当前工作区干净，所有改动都已提交"
```

评委要的就是：**每次改动有记录、随时能回溯**。

## 七、发代码（验收前 1-2 天，自己发）

- 方案 A：`git archive` 打包源码 → 压缩包发 185866315@qq.com
  ```bash
  git archive --format=zip -o 项目名.zip HEAD   # 只含已提交文件，不含 .git
  ```
- 方案 B：推到 GitHub → 发链接（需要 `git remote add` + `git push`）
