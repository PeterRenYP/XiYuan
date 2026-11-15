"""记录析元版本、内置环境、真实验收证据及第三方许可。"""
from __future__ import annotations
import hashlib
import argparse
import importlib.metadata
import json
from pathlib import Path
import shutil


def main():
    """只读取已生成的验收结果，不把未运行测试写成通过。"""
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser()
    parser.add_argument("--output",type=Path,default=root/"dist/ModelObservatoryCpp")
    parser.add_argument("--evidence",type=Path,action="append",default=[])
    args=parser.parse_args();output=args.output.resolve()
    # 每次生成分发目录都复制项目许可证，目录版与安装版使用同一份文本。
    output.mkdir(parents=True,exist_ok=True)
    shutil.copy2(root/"LICENSE",output/"LICENSE")
    notices=output/"third-party-notices";notices.mkdir(parents=True,exist_ok=True)
    distributions=list(importlib.metadata.distributions())+list(importlib.metadata.distributions(path=[str(root/".venv-mcp/Lib/site-packages")]))
    for distribution in distributions:
        for file in distribution.files or []:
            if file.name.lower().startswith(("license","copying","notice")):
                source=Path(distribution.locate_file(file))
                if source.is_file() and source.stat().st_size<5*1024**2:
                    name=distribution.metadata["Name"]+"-"+str(file).replace("/","_").replace("\\","_")+".txt"
                    shutil.copy2(source,notices/name)
    for source in (root/"client-cpp/licenses").glob("*.txt"):shutil.copy2(source,notices/("Qt-"+source.name))
    for directory in ("gcc","winpthreads","mingw-w64"):
        source=root/".cache/qt/Tools/mingw1310_64/licenses"/directory
        if source.is_dir():shutil.copytree(source,notices/("MinGW-"+directory),dirs_exist_ok=True)
    for source in (root/"installer/licenses").glob("*.txt"):shutil.copy2(source,notices/source.name)
    files=list((root/"client-cpp").glob("*.cpp"))+list((root/"client-cpp").glob("*.h"))+list((root/"observatory").glob("*.py"))+list((root/"mcp_server").glob("*.py"))
    files += [output/name for name in ("Xiyuan.exe","Xiyuan-Monitor.exe","Xiyuan-MCP.exe","ModelObservatory-Runtime.exe","_internal/python312.dll")]
    hashes={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files if p.is_file()}
    evidence={}
    # 仅收录显式指定的验收记录，公开分发不自动携带本机历史测试资料。
    for path in args.evidence:
        report=json.loads(path.read_text(encoding="utf-8"))
        if not report.get("passed",report.get("pass",False)):
            raise RuntimeError("验收记录未通过："+str(path))
        evidence[path.resolve().relative_to(root).as_posix()]=report
    manifest={"application":"析元","version":"1.0.4","client":"Qt Widgets 6.8.3 / C++17","runtime":"内置 Python 3.12 / PyTorch 2.9.1 / Transformers 5.3.0",
        "python_install_required":False,"mcp":"Xiyuan-MCP.exe","model_weights_included":False,"hashes":hashes,"validation":evidence,
        "supported_models":["Qwen2 / Qwen2.5 文本模型","Qwen3.5 文本推理"],
        "boundaries":["词表改名仅适配普通 WordLevel 条目，其他分词器只读；向量编辑独立支持", "新增、删除及重排 Token 未开放", "独立文件模式不支持项目推理和调试", "安装器缺少 WebView2 时需联网安装微软界面运行库", "已有 Windows 默认应用受 UserChoice 保护，必要时从系统打开方式选择析元"]}
    (output/"build-manifest.json").write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding="utf-8")
    (output/"使用说明.txt").write_text("""析元 1.0.4

双击 Xiyuan.exe。新建项目，选择本地完整模型目录。程序内置 Python、PyTorch、Transformers、分词器和 Safetensors 依赖，无需另装 Python，也不需要运行开发环境脚本。模型权重不随软件分发。

项目与模型目录分开保存。选择 CPU 或受支持的 NVIDIA 显卡；显卡模式需要兼容驱动。默认不联网下载模型。ModelObservatory-Runtime.exe、_internal、Qt DLL 和插件必须完整保留。

左侧文字导航包含计算图、神经元、激活分析、词表与向量、补丁与版本、权重文件、运行记录和项目首页。左上角显示产品 Logo，标题栏提供文件、编辑及控制菜单；文件菜单统一提供新建项目、打开项目和保存；控制菜单中的神经元干预与右侧操作区共用草稿。计算图页面保留激活空间与注意力视图。索引从 0 开始。

神经元页面提供三维与按层平铺视图，支持旋转、平移、缩放、点击和按层号与神经元编号定位。节点数值来自真实模型与采集记录；空间排布仅供展示。默认无需点击节点，各层显示节点自动按真实激活变色，并轮换播放真实投影耦合光束。每束光独立推进和结束，新激活不会重置在途光束；同时显示的光束数量随绘制负载调整，各层持续轮换参与。左键点击或搜索定位后仅播放该节点；右键点击画布清空选中，恢复全部活动。暂停同时冻结颜色与光束。光束速度为显示效果。平铺视图逐层展示实际神经元数量，点位密集且稳定错落，缩放、选中和定位均使用真实编号；三维视图保留采样显示。

会话支持 Markdown、分段消息、修改会话标题及停止生成。对话可嵌入中心功能区与右侧操作区之间，也可弹出为独立窗口；关闭独立窗口后返回嵌入，保留消息、草稿和状态。使用选择器切换会话，“新建”创建会话。选择器旁的“会话管理”菜单提供修改会话标题、删除会话和删除所有会话，也可右键选择器打开相同菜单。删除前需确认。新建会话默认对话模式。切换会话保留当前主界面页签、视图、布局和窗口位置。

生成期间可以选择其他会话，查看历史消息和已有 Token 分析；主工作区继续显示原任务的实时观测。后台输出始终保存到原生成会话。原任务结束后恢复当前查看会话的发送功能，生成期间的批量删除不可用。

对话输入框支持多行，Enter 发送，Ctrl+Enter 换行。应用记忆嵌入或独立模式。管理窗口的会话列表右键只打开目标菜单，不切换会话或读取其聊天记录。长对话流式生成支持跟随新内容；向上阅读时保留阅读位置。

勾选对话窗口右上角“Token拆分”可显示逐 Token 边框。右键 Token 后点击“分析”查看已有记录中的概率、排名、采样配置和可用的激活关联；分析不会重新推理或修改权重，相关性不等于因果。

张量采集不设项目、单次或单块大小额度；旧项目保存的正数额度不再生效。写盘队列拥堵时先落盘再继续采集，保留实际磁盘预留保护。磁盘不足及模型主动结束回答时使用黄色提示框。

干预面板先编辑规则草稿，再预览并应用。启用复选框与目标列固定在左侧，横向滚动时保持位置。列表勾选不会立即改变模型。临时规则只影响后续输入；修改模型会写回当前权重并生成备份。临时和权重规则统一提交，失败保留上次配置。重复目标包含停用条目，修改自身不冲突。

分支对比支持固定前缀与自由生成。固定前缀比较同一输入下的通道、张量切片、目标 token 概率及排名。自由生成分歧后显示上下文不同。报告记录实际生成配置、精度、环境与规则。

激活分析从会话中选择观测回复，支持正文、思考过程及选中文本，使用实际保存的 Token 边界和采集数据；没有采集的数据不会补零。支持峰值、平均绝对值、频率和跨层归一化排名。切换工作区页面保留已有分析、来源、选区、筛选、排序、页码和选中行。完整排名先筛选排序再分页，不设结果总数上限；可搜索层号、神经元编号及触发 Token，筛选层范围、强度和有效次数。翻页和搜索不重新推理。保存分析导出完整结果。原始数值详情也支持分页。分析期间显示当前阶段、真实处理计数和已用时间；已知总量显示本阶段百分比，未知总量显示滚动进度。等待数据落盘时明确提示，可停止本次任务并保留之前的分析结果。可选择基线、定位候选并加入干预草稿。只采集摘要时不能展开完整值。最后一个未送回模型的 token 没有激活数据。高激活只提供线索。

词表与向量支持文本或编号搜索、特殊 token 筛选、输入输出向量切片及同空间相似查询。向量修改先预览，可保存草稿或提交。输入输出共享权重时只修改一次。受支持的普通 WordLevel 条目可改名；BPE 等未适配词表保留查询，禁止改名。新增、删除和编号重排未开放。

补丁与版本支持撤销、重做、逐条启停、导入和导出。各版本从基座按顺序重放，重叠补丁按顺序作用，不累计重复应用。规则列表和独立导入补丁分别保留归属。备份存放于模型目录的 observatory-backups。

回归页保存目标和常规样例、上下文、生成配置和通过条件。未设置条件显示未评估，失败和不适用分别显示。报告可重新打开。导出并验收会独立加载副本，核对权重、词表与共享关系，并以记录的绝对和相对容差运行样例。临时规则不写入导出的模型。

调试支持层、算子、生成步断点，以及通道或候选 token 条件监视。仅开放架构已支持的采集点。命中后可继续或停止。

打开项目后自动启动独立异常监控进程 Xiyuan-Monitor.exe，不加载模型或占用模型显存。默认每十六个生成步轮换采样一层，最多四个注意力头和一百二十八个激活维度；超出预算进一步降低频率。监控连接异常、样本积压或提示窗口未处理均不会暂停生成。

异常提示在主窗口所在屏幕中央显示，为非模态窗口，按钮为“停止生成”“忽略本次”和“保持忽略”。停止生成取消当前运行；忽略本次仅关闭本次提示；保持忽略在本轮内不再提示同类异常，下轮自动恢复。同类已有待决窗口时不重复弹出。不同类型分别提示。生成结束关闭待决窗口，记录从底部“异常记录”查看。项目 monitor 目录保存有界统计基线与异常记录。

内部偏离检测至少积累三十二份对应指标参考样本后启用，参考按模型、权重、设备、精度、采样与干预设置隔离。基线属于统计参考，不保证参考样本正常。检测覆盖注意力、采样激活与隐藏表示、重复和近似反刍、思考及生成长度、速度和进展。注意力与表示检测为疑似预警，线性注意力仅检测递归状态趋势。没有幻觉、推理正确性或指令遵循检测；未提示不能证明全部位置正常。

独立权重窗口无需项目和推理模型，支持单文件、分片索引、搜索、切片、比较、预览、备份与恢复。文件模式不能使用依赖项目的激活分析和调试。析元独立备份位于实际分片旁的 xiyuan-backups。

Ctrl+S 保存项目。Ctrl++、Ctrl+- 和 Ctrl+0 调整或恢复界面倍率，范围 70%—200%。缩放、布局和应用级窗口位置独立记忆。运行中改权重需先停止任务。

安装器默认安装至 C:\\Program Files\\XiYuan，也可自定义目录。安装与卸载需要管理员权限，支持桌面及开始菜单入口、可选 .safetensors 关联。已有默认应用受 Windows 保护时，从系统“打开方式”选择析元。支持复用安装目录；覆盖已有程序须确认，模型与项目保留。卸载保留用户新增的模型、项目和文件。

MCP 随包提供独立 Xiyuan-MCP.exe。在“帮助 → 安装 MCP”中复制安装提示词，粘贴给 Codex 自动安装并验证连接，提示词使用实际安装路径。连接方法也见 MCP使用说明.txt。新增功能与界面共用后端、版本校验和最终事件，accepted 只表示提交成功。“帮助 → 关于析元”显示实际版本号，目前未提供 OTA 升级。

第三方许可见 third-party-notices。Qt 使用动态链接，允许按许可替换库。Qt 源码版本：https://code.qt.io/cgit/qt/qtbase.git/tag/?h=v6.8.3 。模型使用遵循模型自身许可。
""",encoding="utf-8")
    shutil.copy2(root/"MCP使用说明.txt",output/"MCP使用说明.txt")


if __name__=="__main__":main()
