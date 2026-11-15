"""通过官方 Python MCP SDK 提供标准输入输出工具服务。"""
from __future__ import annotations

import asyncio
import json
import sys
import anyio

from jsonschema import Draft202012Validator, ValidationError
from mcp import types
from mcp.server.lowlevel import Server
from mcp.server.stdio import stdio_server

from .bridge import BridgeClient, BridgeError


def integer(description: str, minimum: int = 0, maximum: int | None = None) -> dict:
    """创建带中文说明与边界的整数参数。"""
    return {"type": "integer", "description": description, "minimum": minimum,
            **({"maximum": maximum} if maximum is not None else {})}


def string(description: str, enum: list[str] | None = None) -> dict:
    """创建中文说明的字符串参数，枚举值保留程序协议名称。"""
    return {"type": "string", "description": description,
            **({"enum": enum} if enum is not None else {"minLength": 1})}


def schema(properties: dict | None = None, required: tuple[str, ...] = ()) -> dict:
    """工具边界拒绝未知字段，防止拼写错误变成隐含默认操作。"""
    return {"type": "object", "properties": properties or {}, "required": list(required),
            "additionalProperties": False}


SELECTION = {
    "branch_id": string("分支编号，省略时使用界面当前分支"),
    "forward_id": string("前向编号，例如 f0；省略时使用界面当前前向"),
    "layer_index": integer("解码层索引，从零开始"),
    "operation": string("算子编号，从 observatory_model_info 返回的 operation_catalog 获取"),
    "token_position": integer("Token位置，从零开始"),
    "neuron_index": integer("神经元通道，从零开始"),
    "head": integer("注意力头索引，从零开始"),
    "dimension": integer("隐藏维度索引，从零开始"),
    "weight_kind": string("投影类型：gate 为门控，up 为上投影，down 为下投影", ["gate", "up", "down"]),
    "lens": {"type": "boolean", "description": "是否计算Token候选透镜"},
    "overview": {"type": "boolean", "description": "是否查询逐层总览"},
}
REVISION = {"expected_weights_revision": string("必须填入最近状态中的 weights_revision，过期版本会被拒绝"),
            "expected_model_revision": string("必须填入最近状态中的 model_revision，防止切换模型后误改另一模型")}
PAGING = {"offset": integer("起始偏移，默认零"), "limit": integer("每页数量，默认二十", 1, 100)}
EVENTS = {"after_cursor": integer("从此事件游标之后读取；操作返回的 event_cursor 可用作起点"),
          "limit": integer("每页最多返回事件数量，默认五十", 1, 100)}
RULE = schema({
    "layer_index": integer("解码层索引，从零开始"),
    "neuron_index": integer("神经元通道索引，从零开始"),
    "kind": string("修改方式：zero 归零；scale 缩放；set 设值；add 加值；steer 归一化引导", ["zero", "scale", "set", "add", "steer"]),
    "value": {"type": "number", "description": "修改数值，必须为模型精度可表示的有限数"},
    "scope": string("临时规则范围：position 指定位置；all 所有位置；generated 生成位置", ["position", "all", "generated"]),
    "token_position": integer("指定Token位置，从零开始，默认零"),
    "projection": string("写入权重时的投影：gate 门控；up 上投影；down 下投影", ["gate", "up", "down"]),
    "dimension": integer("写入权重单个系数的隐藏维度，从零开始"),
}, ("layer_index", "neuron_index", "kind"))

# 工具与本地方法一一对应；异步操作的 accepted 仅表示已提交，最终结果通过事件查询。
TOOLS: dict[str, tuple[str, str, dict, bool]] = {
    "list_instances": ("@list", "列出正在运行且支持 MCP 的客户端。连接信息中的令牌不会返回。", schema(), True),
    "select_instance": ("@select", "选择后续操作的目标客户端。多个客户端时必须先调用此工具。", schema({"instance_id": string("客户端实例编号")}, ("instance_id",)), True),
    "get_state": ("state", "查看当前项目、模型、运行状态、编辑版本、单步边界和事件游标。", schema(), True),
    "model_info": ("model.info", "查看真实模型配置、维度、算子目录和思考模式支持情况。所有索引从零开始。", schema(), True),
    "get_history": ("history", "分页读取界面当前会话及正在生成的回答、模型实际输出的思考文本。", schema(PAGING), True),
    "get_tokens": ("tokens", "分页读取当前运行中的Token，获取准确的Token位置。", schema(PAGING), True),
    "get_events": ("events", "按游标读取加载、输出、编辑、保存和错误事件。dropped 表示早期事件已被环缓冲淘汰，需重新读取状态。", schema(EVENTS, ("after_cursor",)), True),
    "wait_events": ("@wait", "等待指定事件，最长六十秒。异步操作需要等到成功或 request.failed，不能把 accepted 当成完成。", schema({
        "after_cursor": EVENTS["after_cursor"], "names": {"type": "array", "items": {"type": "string"}, "maxItems": 30, "description": "要等待的完整事件名；空数组接受任意事件。错误也应加入 request.failed"},
        "timeout_seconds": {"type": "number", "minimum": 0, "maximum": 60, "description": "等待秒数，默认三十"}}, ("after_cursor", "names")), True),
    "get_telemetry": ("telemetry", "读取客户端最近一次显卡监控数据；空对象表示尚未收到采样。", schema(), True),
    "list_sessions": ("sessions", "列出当前项目会话及当前会话编号。", schema(), True),
    "get_rules": ("rules", "读取当前已应用的临时神经元规则、编辑版本和最近权重备份目录。", schema(), True),
    "query_view": ("view.query", "在客户端显示指定观测位置并查询真实快照；指定神经元时打开神经元页。未采集数据会明确标记。", schema(SELECTION), False),
    "select_view": ("view.select", "同步切换客户端的观测层、算子和Token位置。指定神经元时打开神经元页并定位。运行中切层会改变后续算子采集层。", schema({key: SELECTION[key] for key in ("layer_index", "operation", "neuron_index", "token_position", "dimension", "head")}), False),
    "show_neurons": ("neurons.show", "打开客户端神经元页面，定位节点、恢复全局视图或控制实时播放。可在发送消息前打开。", schema({
        "layer_index": SELECTION["layer_index"], "neuron_index": SELECTION["neuron_index"],
        "global": {"type": "boolean", "description": "清除选中节点，恢复全局视图"},
        "live": {"type": "boolean", "description": "继续或暂停真实活动播放"}}), False),
    "tensor_slice": ("tensor.slice", "按张量引用读取真实捕获值。每个轴都必须提供范围，最多六万五千五百三十六个元素。", schema({
        "tensor_ref": string("query_view 返回的张量引用"),
        "ranges": {"type": "array", "maxItems": 16, "items": schema({"start": integer("起点，包含"), "stop": integer("终点，不包含"), "step": integer("步长", 1)}, ("start", "stop", "step"))},
        "max_elements": integer("最大元素数，默认四千零九十六", 1, 65536)}, ("tensor_ref", "ranges")), True),
    "query_scalar": ("scalar.query", "在客户端定位线性投影，并解释真实捕获值与单精度重算内积。历史权重不可用时返回明确错误。", schema({**SELECTION, "row": integer("投影输入展平后的行索引"), "column": integer("输出列索引")}, ("row", "column")), False),
    "create_project": ("project.create", "新建并打开项目，加载所选本地模型。项目与模型目录必须分开。等待 model.ready 或 request.failed。", schema({
        "directory": string("新项目绝对目录"), "name": string("中文项目名称"), "model_path": string("完整本地模型绝对目录"),
        "device": string("计算设备：cpu 或 cuda:0"), "max_tokens": integer("每次生成上限，默认模型最大可用额度", 1)}, ("directory", "name", "model_path")), False),
    "open_project": ("project.open", "打开已有项目并恢复会话、加载模型。等待 model.ready、session.activated 或 request.failed。", schema({"directory": string("项目绝对目录")}, ("directory",)), False),
    "save_project": ("project.save", "保存项目。运行中会暂停到安全计算边界，等待 checkpoint.saved；空闲时同步保存。", schema(), False),
    "new_session": ("session.new", "在当前项目中新建会话，保留项目级临时神经元规则。", schema(), False),
    "switch_session": ("session.switch", "切换当前项目会话。运行期间拒绝切换；需要时会自动加载目标会话模型。", schema({"session_id": string("list_sessions 返回的会话编号")}, ("session_id",)), False),
    "load_model": ("model.load", "加载或切换项目中的本地模型，等待 model.ready 或 request.failed；切换到不同模型时新建会话。", schema({"path": string("模型绝对目录"), "device": string("计算设备：cpu 或 cuda:0")}, ("path",)), False),
    "send_message": ("run.send", "向界面当前会话发送消息并开始真实推理。返回提交结果，使用 wait_events 等待 run.finished、step.waiting 或 request.failed。", schema({
        "text": {"type": "string", "minLength": 1, "maxLength": 65536, "description": "发送给本地模型的消息"},
        "thinking": {"type": "boolean", "description": "是否开启模型原生深度思考；模型必须支持"},
        "max_tokens": integer("本次最大生成Token数，不覆盖界面默认设置", 1),
        "temperature": {"type": "number", "minimum": 0, "maximum": 2, "description": "采样温度，零表示贪心生成"}}, ("text",)), False),
    "set_mode": ("run.mode", "设置下一条消息的运行模式：normal 对话；observe 观测；step 算子单步。", schema({"mode": string("运行模式", ["normal", "observe", "step"])}, ("mode",)), False),
    "step": ("run.step", "从当前暂停边界执行一个算子，必须携带最新边界身份。", schema({"boundary_id": string("当前计算边界编号"), "state_revision": integer("当前边界版本")}, ("boundary_id", "state_revision")), False),
    "cancel": ("run.cancel", "停止生成或取消模型加载。等待 run.finished 或 model.cancelled 确认停止。", schema(), False),
    "resume_run": ("run.resume", "恢复当前项目保存的运行现场，等待重建和新的暂停边界。", schema(), False),
    "apply_rules": ("edit.apply", "批量修改神经元。temporary 设置后续输入使用的临时规则；model 备份后写回当前模型权重，仅支持归零、缩放、设值。等待 edit.applied 或 request.failed。", schema({
        **REVISION, "target": string("编辑目标：temporary 临时激活；model 当前模型磁盘权重", ["temporary", "model"]),
        "rules": {"type": "array", "minItems": 1, "maxItems": 128, "items": RULE}}, ("expected_weights_revision", "expected_model_revision", "target", "rules")), False),
    "clear_rules": ("edit.clear", "清除临时激活干预。当前模型已写入的权重需要通过备份恢复。等待 edit.applied。", schema(REVISION, ("expected_weights_revision", "expected_model_revision")), False),
    "restore_backup": ("model.restore_backup", "恢复当前模型自己的权重备份，恢复前会创建备份。等待 edit.applied 或 request.failed。", schema({**REVISION, "backup_path": string("当前模型备份绝对目录")}, ("expected_weights_revision", "expected_model_revision", "backup_path")), False),
    "export_model": ("model.export", "将当前模型导出到尚不存在的新目录。等待 model.exported 或 request.failed。", schema({"destination": string("新模型绝对目录")}, ("destination",)), False),
    "open_trace": ("trace.open", "打开已有运行记录用于离线查看。等待 trace.opened，离线状态不能发送对话或修改模型。", schema({"path": string("含 manifest.json 的运行记录绝对目录")}, ("path",)), False),
    "export_trace": ("trace.export", "导出当前观测记录到尚不存在且位于记录外的新目录。等待 trace.exported。", schema({"destination": string("新观测记录绝对目录")}, ("destination",)), False),
}

# 新功能沿用同一个客户端实例、版本和事件游标，不另开隐藏模型。
RULE_DRAFT = schema({**RULE["properties"], "target":string("规则类型",["temporary","model"]),
    "enabled":{"type":"boolean","description":"停用条目仍参与重复检查"}},("target","layer_index","neuron_index","kind","enabled"))
RULES_ARRAY = {"type":"array","maxItems":128,"items":RULE_DRAFT}
NUMBERS = {"type":"array","minItems":1,"maxItems":65536,"items":{"type":"number"}}
VECTOR = {"token_id":integer("Token 编号"),"space":string("向量空间",["input","output"])}
EDIT_VECTOR = {**VECTOR,"start":integer("起点，包含"),"stop":integer("终点，不包含"),"values":NUMBERS,
    "kind":string("修改方式",["set","scale","add","zero"]),"patch_id":string("预览返回的草稿编号，同一编号不会重复应用")}
POINT = schema({"layer_index":integer("层索引，-1 表示输出层",-1),"operation_id":string("算子编号"),
    "generation_step":integer("生成步，从零开始"),"boundary":string("计算前或计算后",["before","after"]),
    "watch":string("监视对象",["channel","token"]),"index":integer("通道或 Token 编号"),
    "comparison":string("比较条件",["gt","ge","lt","le"]),"threshold":{"type":"number"}})
GENERATION = schema({"temperature":{"type":"number","minimum":0,"maximum":2},"top_p":{"type":"number","exclusiveMinimum":0,"maximum":1},
    "top_k":integer("候选数量，零表示不限"),"seed":integer("随机种子"),"max_new_tokens":integer("输出上限",1,64),
    "presence_penalty":{"type":"number","minimum":0,"maximum":2},"repetition_penalty":{"type":"number","exclusiveMinimum":0}})
SAMPLE = schema({"name":string("样例名"),"type":string("目标或常规样例",["target","regular"]),"input":string("本条输入"),
    "messages":{"type":"array","maxItems":128,"items":schema({"role":string("消息角色",["system","user","assistant"]),"content":{"type":"string"}},("role","content"))},
    "target":{"type":"string"},"criterion":string("通过条件",["none","contains","exact","regex","probability","rank"]),
    "expected":{"type":"string"},"threshold":{"type":"number"},"max_probability_drop":{"type":"number","minimum":0},
    "max_new_tokens":integer("输出上限",1,64),"generation":GENERATION})


def feature_tool(name, action, description, properties=None, required=(), readonly=True):
    """所有耗时功能返回请求编号，完成后通过结果工具分页读取。"""
    TOOLS[name] = ("feature."+action,description+" 返回 accepted 后等待 feature.result 或 request.failed，再调用 get_feature_result。",
        schema(properties,required),readonly)


TOOLS.update({
    "get_feature_result":("feature.result","分页读取功能或文件操作的最终结果；过期结果不会伪装成成功。",schema({"request_id":string("提交时返回的请求编号"),"field":string("结果字段，例如 rows、values、patches、targets"),**PAGING},("request_id",)),True),
    "rename_session":("session.rename","修改指定会话主题名，保留当前会话及主窗口状态。",schema({"session_id":string("目标会话编号"),"title":string("新的主题名")},("session_id","title")),False),
    "clear_session":("session.clear","清空当前会话，保留项目规则；运行时拒绝。",schema({"delete_records":{"type":"boolean"}}),False),
    "delete_session":("session.delete","删除当前会话，保留其他会话及项目规则；运行时拒绝。",schema({"delete_records":{"type":"boolean"}}),False),
    "save_rule_draft":("rules.draft","校验并保存完整草稿，不改变生效配置；停用项也检查重复目标。",schema({"rules":RULES_ARRAY},("rules",)),False),
    "configure_debug":("debug.configure","整体替换断点与监视；命中一次后移除。通道使用干预后绝对峰值，Token 使用采样概率。",schema({"points":{"type":"array","maxItems":64,"items":POINT}},("points",)),False),
    "continue_run":("debug.continue","从当前暂停边界继续运行，未命中的监视继续有效。",schema(),False),
})
feature_tool("search_tokens","search","按 Token 文本或编号搜索真实词表。",{"query":{"type":"string"},"exact":{"type":"boolean"},"special":{"type":"boolean"},"offset":integer("起始偏移")})
feature_tool("query_vector","vector","查询输入或输出向量切片、维度、精度与共享关系。",{**VECTOR,"start":integer("切片起点"),"count":integer("切片数量",1,512)},("token_id",))
feature_tool("search_similar_vectors","similar","在同一向量空间查询余弦相近 Token，可提供自定义向量。",{**VECTOR,"query_vector":NUMBERS,"limit":integer("结果数",1,50)},("token_id",))
feature_tool("preview_vector","vector.preview","预览向量修改，不写入模型。",EDIT_VECTOR,("token_id","start","stop","kind","values"))
feature_tool("apply_vector","vector.apply","按预览编号和最新版本写入向量，自动备份。",{**EDIT_VECTOR,**REVISION},("token_id","start","stop","kind","values","patch_id","expected_weights_revision","expected_model_revision"),False)
feature_tool("preview_vocabulary","vocabulary.preview","预览受支持词表条目的修改，不支持的分词器明确拒绝。",{"token_id":integer("Token 编号"),"text":string("新词表文本")},("token_id","text"))
feature_tool("apply_vocabulary","vocabulary.apply","受控修改已有词表条目，保持编号并纳入版本。",{"token_id":integer("Token 编号"),"text":string("新词表文本"),"patch_id":string("预览的草稿编号"),**REVISION},("token_id","text","patch_id","expected_weights_revision","expected_model_revision"),False)
ACTIVATION_PAGING = {"page":integer("页码，从一开始",1),"page_size":integer("每页条数，结果总量不设上限",1,1000),
    "query":{"type":"string","description":"搜索层号、神经元编号或触发 Token"},"layer_min":integer("最小层号"),"layer_max":integer("最大层号"),
    "score_min":{"type":"number","description":"最低强度"},"samples_min":integer("最少有效次数"),
    "sort":string("排序字段",["score","layer","neuron","samples","trigger"]),"ascending":{"type":"boolean","description":"是否升序"}}
feature_tool("analyze_activation","activation","分析真实激活并保存完整排名，缺失样本不参与排名，返回分页结果。",{"scope":string("统计范围",["input","reply","all","range","positions"]),"metric":string("统计方式",["normalized","peak","mean","frequency"]),
    "source":string("干预前后",["baseline","output"]),"layer":integer("层，-1 表示全部",-1),"start":integer("位置起点"),"stop":integer("位置终点"),"baseline":string("可选基线记录目录"),"branch_id":string("来源分支编号"),
    "trace_path":string("所选观测回复的记录目录"),"run_id":string("来源运行编号"),"positions":{"type":"array","items":integer("Token 位置"),"minItems":1},**ACTIVATION_PAGING})
feature_tool("query_activation","activation.query","查询已有完整激活分析，筛选排序后分页，不重新推理或采集。",{"analysis_id":string("分析结果返回的 analysis_id"),**ACTIVATION_PAGING},("analysis_id",))
feature_tool("open_activation","activation.open","打开完整激活分析报告并按页显示。",{"path":string("分析文件路径"),**ACTIVATION_PAGING},("path",))
feature_tool("activation_detail","activation.detail","查看候选通道逐位置的原始值及生成步。",{"layer":integer("层索引"),"neuron":integer("通道索引"),"branch_id":string("分支编号"),"page":integer("页码",1),"page_size":integer("每页位置数",1,1000),"trace_path":string("来源观测记录"),"run_id":string("来源运行编号"),"positions":{"type":"array","items":integer("Token 位置")}},("layer","neuron"))
feature_tool("list_versions","versions","读取补丁来源、顺序和启停状态。")
feature_tool("list_reports","reports","列出当前项目保存的激活、对照与回归报告。")
feature_tool("get_samples","samples.read","读取当前项目保存的验证样例。")
feature_tool("save_samples","samples.save","整体保存项目验证样例，允许空列表。",{"items":{"type":"array","maxItems":100,"items":SAMPLE}},("items",),False)
feature_tool("get_feature_drafts","drafts.read","读取当前项目保存的词表和向量修改草稿。")
feature_tool("save_feature_drafts","drafts.save","校验并整体保存词表和向量草稿，不改变模型。",{"items":{"type":"array","maxItems":128,"items":schema({**EDIT_VECTOR,**REVISION,"action":string("草稿动作",["vector.apply","vocabulary.apply"]),"text":string("新词表文本")},("action","token_id"))}},("items",),False)
feature_tool("open_report","report.read","重开当前项目内的报告；可用结果工具的点分字段路径分页查看。",{"path":string("报告绝对路径")},("path",))
feature_tool("change_version","versions.action","从基座重放补丁，支持撤销、重做和逐条启停。",{**REVISION,"operation":string("版本操作",["undo","redo","toggle"]),"patch_id":string("启停的补丁编号")},("operation","expected_weights_revision","expected_model_revision"),False)
feature_tool("export_patches","versions.export","导出包含基座和词表身份的补丁文件。",{"path":string("新补丁文件绝对路径")},("path",),False)
feature_tool("preview_patches","versions.preview","校验导入兼容性并预览，尚不应用。",{"path":string("补丁文件绝对路径")},("path",))
feature_tool("import_patches","versions.apply","按预览版本导入补丁，同编号不重复应用。",{"path":string("补丁文件绝对路径"),**REVISION},("path","expected_weights_revision","expected_model_revision"),False)
feature_tool("preview_rules","rules.preview","预览混合规则完整提交和实际权重差异。",{"rules":RULES_ARRAY},("rules",))
feature_tool("read_rule_preview_slice","rules.slice","查询规则权重预览中任意展平切片，不写入模型。",{"rules":RULES_ARRAY,"name":string("预览返回的目标张量名"),"start":integer("展平起点"),"count":integer("数量",1,512),**REVISION},("rules","name","expected_weights_revision","expected_model_revision"))
feature_tool("apply_rule_draft","rules.apply","一次提交完整规则列表，删除、停用与清空统一回退；失败保留原配置。",{"rules":RULES_ARRAY,**REVISION},("rules","expected_weights_revision","expected_model_revision"),False)
feature_tool("compare_branches","comparison","用本轮输入重跑固定前缀或自由生成对照，记录首次分歧、激活差值和目标概率排名。",{"mode":string("对照方式",["fixed","free"]),"steps":integer("对照步数",1,64),"target":{"type":"string"},"layer":integer("采集层"),"neuron":integer("候选通道"),"operation":string("采集算子"),"slice_start":integer("张量切片展平起点")})
feature_tool("run_regression","regression","使用同一批项目样例运行原版与编辑版，保存报告及失败原因。",{"samples":{"type":"array","minItems":1,"maxItems":100,"items":SAMPLE}},("samples",),False)
feature_tool("verify_export","export.verify","导出副本后独立加载，核对权重、词表、共享关系及样例结果。",{"samples":{"type":"array","minItems":1,"maxItems":100,"items":SAMPLE},"destination":string("新的模型副本绝对目录")},("samples","destination"),False)
for name,method,description,properties,required,readonly in [
    ("open_weight_file","file.open","独立打开单个 Safetensors 文件或分片索引，不加载推理模型。",{"path":string("权重文件绝对路径")},("path",),True),
    ("list_weight_tensors","file.list","搜索并分页查看张量元数据。",{"query":{"type":"string"},**PAGING},(),True),
    ("read_weight_slice","file.slice","按展平索引读取有界权重切片。",{"name":string("张量名"),"start":integer("起点"),"count":integer("数量",1,65536)},("name",),True),
    ("preview_weight_edit","file.preview","预览文件区间修改及实际存储精度，不写入。",{"name":string("张量名"),"start":integer("起点"),"values":NUMBERS},("name","values"),True),
    ("apply_weight_edit","file.apply","完整备份后原子写入指定区间，必须携带预览版本。",{"name":string("张量名"),"start":integer("起点"),"values":NUMBERS,"expected_revision":string("预览返回的文件版本")},("name","values","expected_revision"),False),
    ("compare_weight_files","file.compare","按块比较同名张量，明确区分缺失、形状、精度和数值差异。",{"other":string("对照文件或分片索引绝对路径"),"name":string("可选张量名"),"atol":{"type":"number","minimum":0},"rtol":{"type":"number","minimum":0}},("other",),True),
    ("restore_weight_file","file.restore","从当前文件相邻的析元备份恢复，恢复前备份当前文件。",{"backup":string("备份文件绝对路径"),"expected_revision":string("当前文件版本")},("backup","expected_revision"),False),
]:
    TOOLS[name]=(method,description+" 等待 file.result 或 request.failed，再读取 get_feature_result。",schema(properties,required),readonly)


def create_server(client: BridgeClient | None = None) -> Server:
    """注册标准 MCP 工具，连接发现与实际操作共用一个明确的实例选择。"""
    bridge = client or BridgeClient()
    server = Server("析元", version="1.0.4", instructions=(
        "控制正在运行的析元客户端。先列出实例、读取状态和模型能力。所有层、神经元和Token索引从零开始。"
        "提交操作前记录事件游标；accepted 仅表示提交，需读取事件确认完成。编辑携带最新权重版本。"
        "工具操作同步显示在析元界面。要实时查看神经元活动，先调用 observatory_show_neurons，再发送消息。"
        "临时规则影响后续输入；写入模型权重会创建备份。避免在其他用户任务运行时切换项目或会话。"))

    @server.list_tools()
    async def list_tools() -> list[types.Tool]:
        """暴露中文描述、严格参数与工具读写标记，便于 Agent 正确调用。"""
        return [types.Tool(name="observatory_" + name, description=description, inputSchema=arguments,
                           annotations=types.ToolAnnotations(readOnlyHint=readonly,
                               destructiveHint=not readonly, openWorldHint=False))
                for name, (_, description, arguments, readonly) in TOOLS.items()]

    @server.call_tool(validate_input=False)
    async def call_tool(name: str, arguments: dict) -> types.CallToolResult:
        """返回结构化结果和中文执行错误，避免异常日志污染协议标准输出。"""
        try:
            key = name.removeprefix("observatory_")
            if not name.startswith("observatory_") or key not in TOOLS:
                raise BridgeError("未知的析元工具", "UNKNOWN_TOOL")
            method, _, arguments_schema, _ = TOOLS[key]
            Draft202012Validator(arguments_schema).validate(arguments)
            # JSON Schema 的 number 不排除 Python 非有限浮点数，发送前额外拒绝。
            try:
                json.dumps(arguments, allow_nan=False)
            except ValueError as exc:
                raise BridgeError("数值必须为有限数", "INVALID_PARAMS") from exc
            if method == "@list":
                data = await bridge.list_instances()
            elif method == "@select":
                data = await bridge.select_instance(arguments["instance_id"])
            elif method == "@wait":
                data = await bridge.wait_events(arguments["after_cursor"], arguments["names"], arguments.get("timeout_seconds", 30))
            else:
                data = await bridge.call(method, arguments)
            result, failed = {"ok": True, "data": data}, False
        except ValidationError as exc:
            result, failed = {"ok": False, "error": {"code": "INVALID_PARAMS", "message": "参数无效，请按工具定义提供字段及取值", "path": list(exc.absolute_path)}}, True
        except BridgeError as exc:
            result, failed = {"ok": False, "error": {"code": exc.code, "message": str(exc)}}, True
        return types.CallToolResult(content=[types.TextContent(type="text", text=json.dumps(result, ensure_ascii=False, allow_nan=False))],
                                    structuredContent=result, isError=failed)

    return server


async def run() -> None:
    """标准输出专用于 MCP 协议，SDK 日志沿标准错误输出。"""
    server = create_server()
    # 复用标准流，避免 SDK 临时 TextIOWrapper 析构时关闭底层管道。
    for stream in (sys.stdin,sys.stdout):
        if hasattr(stream,"reconfigure"):stream.reconfigure(encoding="utf-8")
    async with stdio_server(anyio.wrap_file(sys.stdin),anyio.wrap_file(sys.stdout)) as (reader, writer):
        await server.run(reader, writer, server.create_initialization_options())


def main() -> None:
    """同步命令入口交给异步服务运行，关闭管道后自然退出。"""
    asyncio.run(run())
