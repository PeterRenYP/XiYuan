#include "workbench.h"
#include "uiscale.h"
#include "chattokenview.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QMenu>
#include <QTimer>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QScreen>
#include <QSpinBox>
#include <QTextBrowser>
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QRegularExpression>
#include <QTextOption>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QVBoxLayout>
#include <algorithm>

using namespace Qt::StringLiterals;

void Workbench::showConversation() {
    // 项目选择页与独立权重文件模式不启动对话窗口。
    if (projectDirectory.isEmpty() || property("fileOnly").toBool())
        return;
    setConversationEmbedded(false);
    // 普通顶层窗口可独立最小化；也可从主界面重新唤起。
    conversationWindow->showNormal();
    // 至少让一段标题栏可操作，避免旧多屏坐标只剩一个像素仍被视为可见。
    bool reachable=false;
    const auto title=QRect(conversationWindow->frameGeometry().topLeft(),QSize(conversationWindow->width(),32));
    for(auto *screen:QGuiApplication::screens()){
        const auto visible=screen->availableGeometry().intersected(title);
        if(visible.width()>=120&&visible.height()>=24){reachable=true;break;}
    }
    if(!reachable)if(auto *screen=QGuiApplication::primaryScreen()){
        const auto area=screen->availableGeometry();
        conversationWindow->resize(std::min(conversationWindow->width(),area.width()),std::min(conversationWindow->height(),area.height()));
        conversationWindow->move(area.center()-conversationWindow->rect().center());
    }
    conversationWindow->raise();
    conversationWindow->activateWindow();
}

void Workbench::updateThinkingSupport() {
    // 依据本地分词模板的实际能力启用，不用提示词伪装原生思考模式。
    const bool supported = info.value(u"capabilities"_s).toObject().value(u"thinking_mode"_s).toString() == u"supported"_s;
    thinkingMode->setEnabled(supported && !active && !loading);
    if (!supported)
        thinkingMode->setChecked(false);
    thinkingMode->setToolTip(supported ? u"使用思考采样配置；思考与回答共用生成上限，温度可在模型设置中调整"_s
                                    : u"当前模型不支持原生深度思考模式"_s);
}

void Workbench::setChatHtml(const QString &html, bool forceBottom) {
    // 由聊天视图统一管理即时与延迟排版的滚动恢复，避免旧位置反复覆盖用户操作。
    static_cast<ChatTokenView *>(chat)->setConversationHtml(html, forceBottom);
}

void Workbench::renderChat(bool streamingOnly) {
    // 流式刷新只重排正在生成的回复。切换会话、拆分或折叠时重建历史。
    chatRefresh.stop();
    const bool browsing = !viewedSessionId.isEmpty();
    if (streamingOnly && browsing) return;
    if (!streamingOnly) {
        const auto notices = browsing ? viewedSession.value(u"capture_notices"_s).toObject() : QJsonObject{};
        QStringList text;
        if (browsing) {
            text.append(u"另一会话正在生成。主工作区继续显示该任务的实时数据；当前窗口可查看历史消息与 Token 分析。"_s);
            for (auto it = notices.begin(); it != notices.end(); ++it) text.append(it.value().toString());
        } else text = captureNotices.values();
        captureWarning->setText(text.join(u"\n"_s));
        captureWarning->setVisible(!text.isEmpty());
        captureNoticeFrame->setVisible(!text.isEmpty());
    }
    // 历史展示使用独立快照，后台事件仍只更新成员中的真实生成现场。
    const auto messages = browsing ? viewedSession.value(u"messages"_s).toArray() : this->messages;
    const auto assistantText = browsing ? viewedSession.value(u"assistant_text"_s).toString() : this->assistantText;
    const auto assistantThought = browsing ? viewedSession.value(u"assistant_thought"_s).toString() : this->assistantThought;
    const auto responseTokens = browsing ? viewedSession.value(u"response_tokens"_s).toArray() : this->responseTokens;
    const auto responseGeneration = browsing ? viewedSession.value(u"response_generation"_s).toObject() : this->responseGeneration;
    const auto responseReplyId = browsing ? viewedSession.value(u"response_reply_id"_s).toString() : this->responseReplyId;
    const auto tracePath = browsing ? QDir(projectDirectory).absoluteFilePath(viewedSession.value(u"trace"_s).toString()) : this->tracePath;
    const auto pendingPrompt = browsing ? QString{} : this->pendingPrompt;
    const auto &collapsedThoughts = browsing ? viewedCollapsedThoughts : this->collapsedThoughts;
    const bool responseThinking = browsing ? viewedSession.value(u"response_thinking"_s).toBool() : this->responseThinking;
    const bool responseThinkingComplete = browsing ? viewedSession.value(u"thinking_complete"_s).toBool(true) : this->responseThinkingComplete;
    const bool active = this->active && !browsing;
    const auto escaped = [](const QString &text) { return text.toHtmlEscaped().replace(u"\n"_s, u"<br>"_s); };
    const auto markdown = [](QString text) {
        // 禁用原始 HTML 和图片加载；不完整代码块由同一 Markdown 解析器收尾。
        text.replace(QRegularExpression(u"!\\[([^\\]]*)\\]\\([^)]*\\)"_s), u"\\1"_s);
        QTextDocument document;
        document.setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub).setFlag(QTextDocument::MarkdownNoHTML));
        auto result = document.toHtml();
        const auto start = result.indexOf(u"<body"_s);
        result = result.mid(result.indexOf(QChar(u'>'), start) + 1);
        result.truncate(result.lastIndexOf(u"</body>"_s));
        return result;
    };
    const auto bubble = [](const QString &role, const QString &content, bool user) {
        // 原生富文本表格保留消息底色，Markdown、代码和 Token 锚点仍用同一文档排版。
        return u"<table width='100%' cellspacing='0' cellpadding='10' bgcolor='"_s +
            (user ? u"#344f60"_s : u"#2d343d"_s) +
            u"'><tr><td><p style='color:#a9d8e8'>"_s + role + u"</p>"_s + content +
            u"</td></tr></table><p style='font-size:5px'>&nbsp;</p>"_s;
    };
    QString html;
    const bool split = tokenSplit && tokenSplit->isChecked();
    auto *view = static_cast<ChatTokenView *>(chat);
    view->showTokenBorders = split;
    const auto tokenContent = [&](const QJsonObject &entry, int messageIndex, const QString &channel, const QString &fallback) {
        const auto samples = entry.value(u"output_tokens"_s).toArray();
        if (samples.isEmpty())
            return channel == u"reasoning"_s ? escaped(fallback) : markdown(fallback);
        QString content;
        const auto reply = entry.value(u"reply_id"_s).toString(u"历史回复"_s + QString::number(messageIndex));
        // 普通阅读保持 Markdown 排版，只为确实显示出的源文本片段附加 token 身份。
        QTextDocument formatted;
        struct VisibleRange { int source, document, length; };
        QList<VisibleRange> visibleRanges;
        if (!split) {
            formatted.setHtml(channel == u"reasoning"_s ? escaped(fallback) : markdown(fallback));
            int sourcePosition = 0;
            for (auto block = formatted.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    if (!fragment.isValid() || fragment.text().isEmpty()) continue;
                    const auto found = fallback.indexOf(fragment.text(), sourcePosition);
                    if (found < 0) continue;
                    visibleRanges.append({int(found), fragment.position(), fragment.length()});
                    sourcePosition = int(found) + fragment.length();
                }
        }
        // 两组范围都按文本位置递增，跳过已匹配完的 Markdown 片段。
        qsizetype firstRange = 0;
        QTextCursor formatCursor(&formatted);
        formatCursor.beginEditBlock();
        for (int i = 0; i < samples.size(); ++i) {
            const auto token = samples[i].toObject();
            if (token.value(u"channel"_s).toString() != channel || token.value(u"special"_s).toBool() || token.value(u"hidden"_s).toBool())
                continue;
            auto piece = token.value(u"display_piece"_s).toString(token.value(u"display_text"_s).toString());
            if (piece.isEmpty()) piece = u"空片段"_s;
            const auto anchor = u"token:"_s + reply + u"/"_s + QString::number(i);
            const auto relative = token.value(u"trace"_s).toString(entry.value(u"trace"_s).toString());
            const auto path = relative.isEmpty() ? QString{} : QDir(projectDirectory).absoluteFilePath(relative);
            chatTokenTargets[anchor] = QJsonObject{{u"token"_s, token}, {u"generation"_s, entry.value(u"generation"_s)},
                {u"trace_path"_s, path}, {u"project_root"_s, projectDirectory}, {u"reply_id"_s, reply},
                {u"prompt_text"_s, entry.value(u"prompt_text"_s).toString().left(4096)}};
            const auto tip = u"编号 "_s + QString::number(token.value(u"id"_s).toInt()) + u"；生成位置 "_s +
                QString::number(token.value(u"generation_step"_s).toInt()) + u"；关联文本："_s + token.value(u"joined_text"_s).toString(piece);
            if (!split) {
                // 跨字节的共享字符在普通排版中绑定首个 token；拆分后每个续片均可操作。
                const auto shared = token.value(u"shared_tokens"_s).toArray();
                if (!shared.isEmpty() && shared.first() != token.value(u"generation_step"_s)) continue;
                if (!token.contains(u"text_start"_s) || !token.contains(u"text_end"_s)) continue;
                const int start = token.value(u"text_start"_s).toInt(), end = token.value(u"text_end"_s).toInt();
                while (firstRange < visibleRanges.size() &&
                       visibleRanges[firstRange].source + visibleRanges[firstRange].length <= start)
                    ++firstRange;
                for (qsizetype rangeIndex = firstRange; rangeIndex < visibleRanges.size(); ++rangeIndex) {
                    const auto &range = visibleRanges[rangeIndex];
                    if (range.source >= end) break;
                    const int left = std::max(start, range.source), right = std::min(end, range.source + range.length);
                    if (left >= right) continue;
                    formatCursor.setPosition(range.document + left - range.source);
                    formatCursor.setPosition(range.document + right - range.source, QTextCursor::KeepAnchor);
                    QTextCharFormat format;
                    format.setAnchor(true);
                    format.setAnchorHref(anchor);
                    format.setFontUnderline(false);
                    format.setToolTip(tip);
                    formatCursor.mergeCharFormat(format);
                }
                continue;
            }
            const bool newline = piece.contains(QChar(u'\n'));
            auto visible = piece.toHtmlEscaped().replace(u" "_s, u"&nbsp;"_s).replace(u"\t"_s, u"&nbsp;&nbsp;&nbsp;&nbsp;"_s)
                .replace(u"\r"_s, QString{}).replace(u"\n"_s, u"&nbsp;换行&nbsp;"_s);
            content += u"<a style='color:#d8e8f2;text-decoration:none' href='"_s + anchor + u"' title='"_s + tip.toHtmlEscaped() +
                u"'>"_s + visible + u"</a>&#8202;"_s + (newline ? u"<br>"_s : QString{});
        }
        formatCursor.endEditBlock();
        if (!split) {
            auto result = formatted.toHtml();
            const auto body = result.indexOf(u"<body"_s);
            result = result.mid(result.indexOf(QChar(u'>'), body) + 1);
            result.truncate(result.lastIndexOf(u"</body>"_s));
            return result;
        }
        return content.isEmpty() ? (channel == u"reasoning"_s ? escaped(fallback) : markdown(fallback)) : content;
    };
    auto response = [&](const QJsonObject &entry, int index, bool streaming) {
        const auto thought = entry.value(u"reasoning_content"_s).toString();
        const bool complete = entry.value(u"thinking_complete"_s).toBool(true);
        if (!thought.isEmpty() || (streaming && responseThinking)) {
            const bool collapsed = collapsedThoughts.contains(index);
            html += u"<p style='color:#aebed2'><a style='color:#a6c7dd' href='thought:"_s + QString::number(index) +
                u"'>"_s + (collapsed ? u"展开思考过程"_s : u"收起思考过程"_s) + u"</a>　"_s +
                (streaming && !complete ? u"正在思考"_s : complete ? u"思考已结束"_s : u"思考未完成"_s) + u"</p>"_s;
            if (!collapsed)
                html += u"<blockquote style='color:#acbcd0'>"_s + (thought.isEmpty() ? u"等待模型输出思考文本…"_s : tokenContent(entry, index, u"reasoning"_s, thought)) + u"</blockquote>"_s;
        }
        const auto content = entry.value(u"content"_s).toString();
        if (!content.isEmpty())
            html += bubble(u"模型"_s, tokenContent(entry, index, u"answer"_s, content), false);
        if (split && entry.value(u"output_tokens"_s).toArray().isEmpty() && !content.isEmpty())
            html += u"<p style='color:#b9c6d2'>该历史回复未保存实际 Token 边界，无法逐 Token 分析。</p>"_s;
    };
    if (!streamingOnly) {
        chatTokenTargets.clear();
        for (int i = 0; i < messages.size(); ++i) {
            const auto entry = messages[i].toObject();
            if (entry.value(u"role"_s).toString() == u"assistant"_s)
                response(entry, i, false);
            else
                html += bubble(
                    (entry.value(u"role"_s).toString() == u"user"_s ? u"用户"_s : u"系统"_s) +
                    (entry.contains(u"mode"_s) ? entry.value(u"mode"_s).toString() == u"normal"_s ? u" · 对话"_s : u" · 观测"_s : QString{}) +
                    QString{}, markdown(entry.value(u"content"_s).toString()), entry.value(u"role"_s).toString() == u"user"_s);
        }
        setChatHtml(html.isEmpty() ? sessionId.isEmpty() && !browsing ? u"暂无会话，点击“新建”开始。"_s : u"新会话。选择对话或观测模式，输入消息开始。"_s : html);
        html.clear();
    }
    const auto last = messages.isEmpty() ? QJsonObject{} : messages.last().toObject();
    const bool committed = last.value(u"role"_s).toString() == u"assistant"_s &&
        last.value(u"content"_s).toString() == assistantText && last.value(u"reasoning_content"_s).toString() == assistantThought;
    if (!committed && (!assistantText.isEmpty() || !assistantThought.isEmpty() || (active && responseThinking)))
        response({{u"content"_s, assistantText}, {u"reasoning_content"_s, assistantThought},
                  {u"thinking_complete"_s, responseThinkingComplete}, {u"reply_id"_s, responseReplyId},
                  {u"output_tokens"_s, responseTokens}, {u"generation"_s, responseGeneration},
                  {u"trace"_s, tracePath.isEmpty() ? QString{} : QDir(projectDirectory).relativeFilePath(tracePath)},
                  {u"prompt_text"_s, pendingPrompt}}, messages.size(), active);
    view->setStreamingHtml(html);
}

void Workbench::showCaptureNotice(const QString &key, const QString &text) {
    // 不同来源分别保存，采集提示与模型结束提示不会互相覆盖。
    if (text.isEmpty()) return;
    captureNotices[key] = text;
    if (!viewedSessionId.isEmpty()) return;
    captureWarning->setText(QStringList(captureNotices.values()).join(u"\n"_s));
    captureWarning->show();
    captureNoticeFrame->show();
}

void Workbench::clearCaptureNotices() {
    // 新运行清空上次状态，保留当前运行随后到达的各类提示。
    captureNotices.clear();
    if (!viewedSessionId.isEmpty()) return;
    captureWarning->clear();
    captureWarning->hide();
    captureNoticeFrame->hide();
}

void Workbench::showTokenMenu(const QString &anchor, const QPoint &position) {
    // 右键只显示菜单；点击分析时才读取记录并打开面板。
    if (!chatTokenTargets.contains(anchor)) return;
    const auto target = chatTokenTargets.value(anchor);
    auto *menu = new QMenu(chat);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(u"分析"_s, this, [this, target] { analyzeToken(target); });
    menu->popup(position);
}

void Workbench::analyzeToken(const QJsonObject &target) {
    // 面板与请求绑定不可变的来源，即使用户切换会话也不更换分析对象。
    auto *dialog = new QDialog(conversationWindow);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(u"输出 Token 分析"_s);
    dialog->resize(880, 720);
    auto *layout = new QVBoxLayout(dialog);
    auto *body = new QTextBrowser;
    body->setOpenLinks(false);
    body->setOpenExternalLinks(false);
    layout->addWidget(body, 1);
    auto *closeButton = new QPushButton(u"关闭"_s);
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
    layout->addWidget(closeButton);
    QJsonObject initial{{u"token"_s, target.value(u"token"_s)}, {u"generation"_s, target.value(u"generation"_s)},
        {u"context"_s, target.value(u"prompt_text"_s)}, {u"explanation"_s, u"正在读取该次生成的采样与观测数据…"_s}};
    body->setHtml(tokenAnalysisHtml(initial));
    dialog->show();
    if (!runtimeEnabled) {
        initial[u"explanation"_s] = u"运行器未启动，当前仅显示会话保存的数据。"_s;
        body->setHtml(tokenAnalysisHtml(initial));
        return;
    }
    ensureProcesses();
    connect(&worker, &Transport::message, dialog, [this, dialog, body](const QJsonObject &message) {
        if (message.value(u"request_id"_s).toString() != dialog->property("analysisRequest").toString()) return;
        if (message.value(u"kind"_s).toString() == u"error"_s)
            body->setPlainText(u"分析失败："_s + message.value(u"payload"_s).toObject().value(u"message"_s).toString());
        else if (message.value(u"name"_s).toString() == u"token.analysis"_s)
            body->setHtml(tokenAnalysisHtml(message.value(u"payload"_s).toObject()));
    });
    connect(&worker, &Transport::failed, dialog, [body, dialog](const QString &error) {
        if (dialog->property("analysisPending").toBool()) body->setPlainText(u"分析中断："_s + error);
        dialog->setProperty("analysisPending", false);
    });
    connect(&worker, &Transport::stopped, dialog, [body, dialog](int) {
        if (dialog->property("analysisPending").toBool()) body->setPlainText(u"运行器已退出，请重新打开分析。"_s);
        dialog->setProperty("analysisPending", false);
    });
    const auto request = worker.send(u"token.analyze"_s, target);
    dialog->setProperty("analysisRequest", request);
    tokenAnalysisRequests.insert(request);
    QTimer::singleShot(60000, dialog, [body, dialog] {
        if (dialog->property("analysisPending").toBool())
            body->append(u"读取观测数据仍未完成，可以关闭面板后重试。"_s);
    });
    dialog->setProperty("analysisPending", true);
    connect(&worker, &Transport::message, dialog, [dialog, request](const QJsonObject &message) {
        if (message.value(u"request_id"_s).toString() == request) dialog->setProperty("analysisPending", false);
    });
}

QString Workbench::tokenAnalysisHtml(const QJsonObject &report) const {
    // 所有动态内容转义，内部字段在此统一转换为中文标签。
    const auto escape = [](const QString &value) { return value.toHtmlEscaped().replace(u"\n"_s, u"<br>"_s); };
    const auto number = [](const QJsonValue &value) { return value.isDouble() ? QString::number(value.toDouble(), 'g', 8) : u"未保存"_s; };
    const auto token = report.value(u"token"_s).toObject();
    const auto generation = report.value(u"generation"_s).toObject();
    const auto branchId = token.value(u"branch_id"_s).toString();
    const auto branchText = branchId == u"base"_s ? u"原始分支"_s : branchId.isEmpty() ? u"未保存"_s : u"编辑分支 "_s + branchId;
    QString html = u"<h2>输出 Token 分析</h2><p>所选片段：<b>"_s + escape(token.value(u"display_piece"_s).toString(token.value(u"display_text"_s).toString())) +
        u"</b><br>编号："_s + number(token.value(u"id"_s)) + u"　生成位置："_s + number(token.value(u"generation_step"_s)) +
        u"　上下文位置："_s + number(token.value(u"position"_s)) + u"</p><p>运行编号："_s + escape(token.value(u"run_id"_s).toString()) +
        u"<br>前向编号："_s + escape(token.value(u"forward_id"_s).toString()) + u"　分支："_s + escape(branchText) +
        u"<br>模型版本："_s + escape(token.value(u"model_revision"_s).toString(u"未保存"_s)) + u"　权重版本："_s + escape(token.value(u"weights_revision"_s).toString(u"未保存"_s)) +
        u"<br>关联文本："_s + escape(token.value(u"joined_text"_s).toString(token.value(u"display_piece"_s).toString())) + u"</p>"_s;
    html += u"<h3>自动分析</h3><p>"_s + escape(report.value(u"explanation"_s).toString()) + u"</p>"_s;
    html += u"<h3>采样依据</h3><p>实际采样概率："_s + (token.value(u"probability"_s).isDouble() ? QString::number(token.value(u"probability"_s).toDouble() * 100, 'f', 5) + u"%"_s : u"未保存"_s) +
        u"　采样排名："_s + number(token.value(u"sampling_rank"_s)) + u"<br>原始词表分数："_s + number(token.value(u"raw_logit"_s)) +
        u"　原始排名："_s + number(token.value(u"raw_rank"_s)) + u"<br>原始词表概率："_s + number(token.value(u"raw_probability"_s)) +
        u"　调整后分数："_s + number(token.value(u"adjusted_score"_s)) + u"</p>"_s;
    html += u"<h3>采样设置</h3><p>温度："_s + number(generation.value(u"temperature"_s)) + u"　候选数量限制："_s + number(generation.value(u"top_k"_s)) +
        u"　累计概率阈值："_s + number(generation.value(u"top_p"_s)) + u"<br>存在惩罚："_s + number(generation.value(u"presence_penalty"_s)) +
        u"　重复惩罚："_s + number(generation.value(u"repetition_penalty"_s)) + u"　随机种子："_s + number(generation.value(u"seed"_s)) + u"</p>"_s;
    html += u"<h3>候选对比</h3><table cellspacing='0' cellpadding='5' border='1'><tr><th>编号</th><th>文本</th><th>实际采样概率</th><th>原始词表分数</th></tr>"_s;
    for (const auto &value : token.value(u"candidates"_s).toArray()) {
        const auto candidate = value.toObject();
        html += u"<tr><td>"_s + number(candidate.value(u"id"_s)) + u"</td><td>"_s + escape(candidate.value(u"text"_s).toString()) + u"</td><td>"_s + number(candidate.value(u"probability"_s)) + u"</td><td>"_s + number(candidate.value(u"raw_logit"_s)) + u"</td></tr>"_s;
    }
    html += u"</table><p>候选表展示概率最高的部分候选，所选 Token 的数值始终单独列于上方。</p>"_s;
    html += u"<h3>输出通道贡献</h3><table cellspacing='0' cellpadding='5' border='1'><tr><th>通道序号</th><th>方向</th><th>贡献值</th></tr>"_s;
    const auto projection = token.value(u"projection"_s).toObject();
    for (const auto &key : {u"positive"_s, u"negative"_s})
        for (const auto &value : projection.value(key).toArray()) {
            const auto contribution = value.toObject();
            html += u"<tr><td>"_s + number(contribution.value(u"index"_s)) + u"</td><td>"_s + (key == u"positive"_s ? u"提高分数"_s : u"降低分数"_s) + u"</td><td>"_s + number(contribution.value(u"value"_s)) + u"</td></tr>"_s;
        }
    html += u"</table><p>全部通道贡献与偏置之和："_s + number(projection.value(u"sum"_s)) + u"　偏置："_s + number(projection.value(u"bias"_s)) + u"<br>贡献按单精度分解，与原始低精度词表分数可能存在舍入差异。</p><h3>预测前向的层激活</h3>"_s;
    for (const auto &value : report.value(u"layers"_s).toArray()) {
        const auto layer = value.toObject();
        html += u"<p>层序号 "_s + number(layer.value(u"layer"_s)) + u"　均方根："_s + number(layer.value(u"rms"_s)) + u"<br>高响应通道："_s;
        for (const auto &row : layer.value(u"channels"_s).toArray())
            html += number(row.toObject().value(u"index"_s)) + u"（"_s + number(row.toObject().value(u"value"_s)) + u"）　"_s;
        html += u"</p>"_s;
    }
    html += u"<h3>上下文关联</h3><p>本轮输入："_s + escape(report.value(u"context"_s).toString()) + u"</p>"_s;
    for (const auto &value : report.value(u"attention"_s).toArray()) {
        const auto attention = value.toObject();
        html += u"<p>层序号 "_s + number(attention.value(u"layer"_s)) + u"（各注意力头的平均权重）</p>"_s;
        for (const auto &row : attention.value(u"positions"_s).toArray()) {
            const auto position = row.toObject();
            html += u"<p>位置 "_s + number(position.value(u"position"_s)) + u"："_s + escape(position.value(u"text"_s).toString()) + u"　权重 "_s + number(position.value(u"weight"_s)) + u"</p>"_s;
        }
    }
    for (const auto &note : report.value(u"notes"_s).toArray())
        html += u"<p style='color:#e3ca82'>"_s + escape(note.toString()) + u"</p>"_s;
    html += u"<p>数据来源："_s + escape(report.value(u"evidence"_s).toString(u"会话保存的生成数据"_s)) + u"。层、通道及位置序号从零开始。</p>"_s;
    return html;
}

void Workbench::updateOutputLimit() {
    // 模型上下文决定可设置上限；实际发送时由分词后的剩余额度进一步约束。
    const bool wasFull = maximum->value() == maximum->maximum();
    const auto config = info.value(u"config"_s).toObject();
    maximum->setMaximum(std::max(1, config.value(u"max_position_embeddings"_s).toInt(262144) - 1));
    if (wasFull)
        maximum->setValue(maximum->maximum());
}

void Workbench::beginModelLoading() {
    // 发送模型命令前立即展示；不能测量的初始化阶段使用忙碌进度。
    finishModelLoading();
    loadingDialog = new QDialog(this, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
    loadingDialog->setWindowTitle(u"正在加载模型"_s);
    loadingDialog->setWindowModality(Qt::ApplicationModal);
    UiScale::minimumWidth(loadingDialog, 540);
    auto *layout = new QVBoxLayout(loadingDialog);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    auto *path = new QLabel(u"模型目录："_s + modelPath);
    path->setWordWrap(true);
    layout->addWidget(path);
    loadingStep = new QLabel(u"正在启动运行器并检查模型配置"_s);
    loadingStep->setWordWrap(true);
    layout->addWidget(loadingStep);
    loadingProgress = new QProgressBar;
    loadingProgress->setObjectName(u"modelLoadingProgress"_s);
    loadingProgress->setRange(0, 0);
    layout->addWidget(loadingProgress);
    auto *cancelLoad = new QPushButton(u"取消加载"_s);
    layout->addWidget(cancelLoad);
    connect(cancelLoad, &QPushButton::clicked, this, [this, cancelLoad] {
        cancel();
        cancelLoad->setEnabled(false);
        loadingStep->setText(u"正在等待当前加载步骤安全结束，请稍候"_s);
    });
    loadingDialog->show();
    loadingDialog->raise();
}

void Workbench::updateModelLoading(const QJsonObject &payload) {
    // 百分比只代表当前可测量步骤，切到设备传输时恢复忙碌显示。
    if (!loadingDialog || !loading)
        return;
    loadingStep->setText(payload.value(u"message"_s).toString());
    const double total = payload.value(u"total"_s).toDouble();
    if (total > 0) {
        loadingProgress->setRange(0, 100);
        loadingProgress->setValue(qBound(0, qRound(payload.value(u"completed"_s).toDouble() * 100 / total), 100));
        loadingProgress->setFormat(u"当前步骤 %p%"_s);
    } else {
        loadingProgress->setRange(0, 0);
    }
}

void Workbench::finishModelLoading() {
    // 成功、失败、取消或运行器退出时均释放进度窗口。
    if (!loadingDialog)
        return;
    loadingDialog->hide();
    loadingDialog->deleteLater();
    loadingDialog = nullptr;
    loadingProgress = nullptr;
    loadingStep = nullptr;
}
