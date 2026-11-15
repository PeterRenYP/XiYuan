#include "anomalyengine.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <numeric>

using namespace Qt::StringLiterals;

namespace {
// 稳健分位数与偏差可抵抗单次尖峰；所有输入均为有界小数组。
double median(QList<double> values) {
    if (values.isEmpty()) return 0;
    std::sort(values.begin(), values.end());
    const int n = int(values.size());
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) * .5;
}
double deviation(const QList<double> &values) {
    const double center = median(values);
    QList<double> distances;
    for (double value : values) distances.append(std::abs(value - center));
    return median(distances) * 1.4826;
}
QList<double> numbers(const QJsonArray &values) {
    QList<double> result;
    for (const auto &value : values) if (value.isDouble() && std::isfinite(value.toDouble())) result.append(value.toDouble());
    return result;
}
double cosine(const QList<double> &a, const QList<double> &b) {
    if (a.size() != b.size() || a.isEmpty()) return 0;
    double dot = 0, aa = 0, bb = 0;
    for (int i = 0; i < a.size(); ++i) { dot += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
    return aa > 0 && bb > 0 ? dot / std::sqrt(aa * bb) : 0;
}
double jensenShannon(const QList<double> &a, const QList<double> &b) {
    if (a.size() != b.size() || a.isEmpty()) return 1;
    double result = 0;
    for (int i = 0; i < a.size(); ++i) {
        const double middle = (a[i] + b[i]) * .5;
        if (a[i] > 0 && middle > 0) result += .5 * a[i] * std::log(a[i] / middle);
        if (b[i] > 0 && middle > 0) result += .5 * b[i] * std::log(b[i] / middle);
    }
    return result;
}
// 词组指纹只识别文本近似重复，不把它表述为语义或事实判定。
double shingles(const QString &a, const QString &b) {
    QSet<QString> x, y;
    for (int i = 0; i + 4 <= a.size(); i += 2) x.insert(a.mid(i, 4));
    for (int i = 0; i + 4 <= b.size(); i += 2) y.insert(b.mid(i, 4));
    if (x.isEmpty() || y.isEmpty()) return 0;
    const auto intersection = QSet<QString>(x).intersect(y).size();
    return double(intersection) / double(x.size() + y.size() - intersection);
}
}

QString anomalyName(const QString &type) {
    // 存储字段使用英文，所有可见类型均在此转换为中文。
    static const QMap<QString, QString> names{
        {u"attention_collapse"_s, u"疑似注意力过度集中"_s}, {u"head_convergence"_s, u"疑似注意力头趋同"_s},
        {u"activation_growth"_s, u"激活持续放大"_s}, {u"activation_decay"_s, u"激活持续衰减"_s},
        {u"representation_decay"_s, u"疑似隐藏表示退化"_s}, {u"repetition"_s, u"重复循环"_s},
        {u"rumination"_s, u"疑似反复推演"_s}, {u"overthinking"_s, u"疑似过度思考"_s},
        {u"unfinished_thinking"_s, u"思考区段未完成"_s}, {u"generation_limit"_s, u"长时间生成或接近生成上限"_s},
        {u"slow_generation"_s, u"生成速度持续下降"_s}, {u"no_progress"_s, u"长时间无生成进展"_s},
        {u"unresponsive"_s, u"生成进程监控连接无响应"_s}, {u"nonfinite"_s, u"采样数值无效"_s}};
    return names.value(type, u"未分类生成异常"_s);
}

AnomalyEngine::AnomalyEngine(std::function<void(const QJsonObject &)> output) : output(std::move(output)) {
    // 仅保存输出回调，所有状态均为普通 CPU 数据。
}

void AnomalyEngine::emitEvent(const QString &name, const QJsonObject &payload) {
    // 所有结果携带运行与分支身份，界面丢弃过期告警。
    output({{u"name"_s, name}, {u"run_id"_s, run}, {u"branch_id"_s, branch}, {u"payload"_s, payload}});
}

QString AnomalyEngine::referenceKey(const QString &metric, int layer, const QString &phase, int context) const {
    // 对上下文长度按宽区间分桶，避免低频采样永远无法积累足够参考。
    const int bucket = context <= 128 ? 0 : context <= 1024 ? 1 : context <= 8192 ? 2 : 3;
    return profile + u"/"_s + phase + u"/"_s + QString::number(bucket) + u"/"_s + QString::number(layer) + u"/"_s + metric;
}

QList<double> AnomalyEngine::reference(const QString &key) const {
    auto values = baselines.value(key).values;
    // 冷启动只用于观察；至少三十二份样本后才启动基线偏离告警。
    if (values.size() < 32) values += candidates.value(key).values;
    return values;
}

void AnomalyEngine::learn(const QString &key, double value, bool suspect) {
    // 成熟参考冻结；异常窗口、数据缺口与不完整运行不能污染持久基线。
    if (suspect || transportGap || !std::isfinite(value) || baselines.value(key).values.size() >= 64) return;
    if (!candidates.contains(key) && candidates.size() >= 512) return;
    auto &values = candidates[key].values;
    if (values.size() < 64) values.append(value);
}

void AnomalyEngine::condition(const QString &key, const QString &type, bool bad, const QJsonObject &evidence, int required) {
    // 每个位置独立积累，按异常类型合并事件；连续恢复后才允许再次提示。
    if (!gates.contains(key) && gates.size() >= 4096) { transportGap = true; return; }
    auto &gate = gates[key]; gate.type = type;
    if (bad) { ++gate.bad; gate.good = 0; }
    else { gate.bad = 0; ++gate.good; }
    if (!gate.active && gate.bad >= required) {
        gate.active = true; hadAlert = true;
        if (!announced.contains(type)) {
            announced.insert(type);
            auto payload = evidence;
            payload[u"type"_s] = type; payload[u"title"_s] = anomalyName(type);
            payload[u"step"_s] = evidence.contains(u"sample_step"_s) ? evidence.value(u"sample_step"_s) : QJsonValue(lastStep);
            payload[u"consecutive"_s] = gate.bad;
            payload[u"suppressed"_s] = ignored.contains(type);
            emitEvent(u"anomaly.detected"_s, payload);
        }
    }
    if (gate.active && gate.good >= 3) {
        gate.active = false;
        bool other = false;
        for (const auto &entry : gates) if (entry.type == type && entry.active) { other = true; break; }
        if (!other && announced.remove(type)) emitEvent(u"anomaly.recovered"_s, {{u"type"_s, type}});
    }
}

void AnomalyEngine::accept(const QJsonObject &message, qint64 now) {
    // 按运行与分支身份处理数据，旧进程的迟到消息不能改变新一轮检测。
    const auto kind = message.value(u"kind"_s).toString();
    if (kind == u"start"_s) {
        // 新运行或新分支重新建立窗口；忽略状态不跨生成保留。
        active = true; waiting = hadAlert = transportGap = false;
        run = message.value(u"run_id"_s).toString(); branch = message.value(u"branch_id"_s).toString();
        profile = message.value(u"profile"_s).toString(); metadata = message;
        gates.clear(); windows.clear(); candidates.clear(); ignored.clear(); announced.clear();
        recentTokens.clear(); reasoning.clear(); answer.clear(); progressIdentity.clear();
        lastStep = -1; reasoningTokens = validTensorSamples = dropped = droppedTokens = sampled = eligible = slowStreak = 0;
        started = lastHeartbeat = lastProgress = now; lastStatus = now - 1000;
        loadReferences(message.value(u"project_root"_s).toString());
        emitEvent(u"anomaly.started"_s, {{u"baseline"_s, u"统计参考积累中；未验证样本正常性"_s}});
        return;
    }
    if (!active || message.value(u"run_id"_s).toString() != run || message.value(u"branch_id"_s).toString() != branch) return;
    if (kind == u"feedback"_s) {
        if (message.value(u"ignore"_s).toBool()) ignored.insert(message.value(u"type"_s).toString());
        return;
    }
    if (kind == u"pause"_s) {
        waiting = message.value(u"waiting"_s).toBool(); lastProgress = lastHeartbeat = now; return;
    }
    lastHeartbeat = now;
    if (kind == u"transport_lost"_s) {
        transportGap = true; status(now); return;
    }
    if (kind == u"progress"_s) {
        const bool wasWaiting = waiting;
        waiting = message.value(u"waiting"_s).toBool();
        const auto identity = QString::number(message.value(u"step"_s).toInt()) + u":"_s +
            QString::number(message.value(u"layer"_s).toInt()) + u":"_s + message.value(u"operation"_s).toString();
        if (identity != progressIdentity || waiting || wasWaiting) { lastProgress = now; progressIdentity = identity; }
        if (message.value(u"dropped"_s).toInt() > dropped) {
            transportGap = true;
            // 缺失样本使连续条件失效，不能跨缺口拼接异常证据。
            for (auto it = gates.begin(); it != gates.end(); ++it) { it->bad = it->good = 0; }
        }
        if (message.value(u"dropped_tokens"_s).toInt() > droppedTokens) {
            recentTokens.clear(); reasoning.clear(); answer.clear();
            droppedTokens = message.value(u"dropped_tokens"_s).toInt();
        }
        dropped = message.value(u"dropped"_s).toInt(); sampled = message.value(u"sampled"_s).toInt(); eligible = message.value(u"eligible"_s).toInt();
        if (message.value(u"capture_disabled"_s).toBool()) transportGap = true;
    } else if (kind == u"token"_s) token(message, now);
    else if (kind == u"tensor"_s) tensor(message);
    else if (kind == u"finish"_s) finish(message, now);
}

void AnomalyEngine::tensor(const QJsonObject &message) {
    // 只消费有界 CPU 摘要，按层和采样阶段独立计算指标。
    const auto operation = message.value(u"operation"_s).toString();
    const int layer = message.value(u"layer"_s).toInt(), step = message.value(u"step"_s).toInt();
    const int context = message.value(u"position"_s).toInt() + 1;
    const auto phase = message.value(u"phase"_s).toString();
    const auto id = QString::number(layer) + u"/"_s + phase + u"/"_s + operation;
    QJsonObject evidence{{u"layer"_s, layer}, {u"sample_step"_s, step}, {u"sampled"_s, true}};
    ++validTensorSamples;
    if (operation == u"attention"_s) {
        const auto heads = message.value(u"heads"_s).toArray(), rows = message.value(u"values"_s).toArray();
        QList<QList<double>> distributions;
        for (int i = 0; i < std::min(heads.size(), rows.size()); ++i) {
            const auto row = numbers(rows[i].toArray()); if (row.size() < 5) continue;
            const int head = heads[i].toInt();
            const auto key = id + u"/"_s + QString::number(head);
            if (!windows.contains(key) && windows.size() >= 2048) { transportGap = true; continue; }
            auto &window = windows[key];
            const int position = int(row[2]); window.fixed = position == window.position ? window.fixed + 1 : 1; window.position = position;
            const auto refKey = referenceKey(u"entropy/"_s + QString::number(head), layer, phase, context);
            const auto ref = reference(refKey); const bool mature = ref.size() >= 32;
            const bool bad = context >= 16 && row[3] > .5 && mature && row[0] < .12 && row[1] > .9 && window.fixed >= 3 &&
                             row[0] < median(ref) - std::max(.08, 6 * deviation(ref));
            auto detail = evidence; detail[u"head"_s] = head; detail[u"entropy"_s] = row[0]; detail[u"peak"_s] = row[1]; detail[u"key_position"_s] = position;
            detail[u"baseline_median"_s] = median(ref); detail[u"baseline_samples"_s] = ref.size();
            condition(key + u"/collapse"_s, u"attention_collapse"_s, bad, detail);
            condition(key + u"/finite"_s, u"nonfinite"_s, row[3] < .5, detail, 1);
            learn(refKey, row[0], bad || row[3] < .5);
            distributions.append(row.mid(4));
        }
        if (distributions.size() >= 2 && context >= 16) {
            double distance = 0; int pairs = 0;
            for (int i = 0; i < distributions.size(); ++i) for (int j = i + 1; j < distributions.size(); ++j) {
                distance += jensenShannon(distributions[i], distributions[j]); ++pairs;
            }
            distance /= pairs;
            const auto key = referenceKey(u"head_distance"_s, layer, phase, context); const auto ref = reference(key);
            const bool bad = ref.size() >= 32 && distance < .005 && distance < median(ref) - std::max(.01, 6 * deviation(ref));
            evidence[u"distance"_s] = distance; evidence[u"baseline_median"_s] = median(ref); evidence[u"approximate"_s] = true;
            condition(id + u"/heads"_s, u"head_convergence"_s, bad, evidence);
            learn(key, distance, bad);
        }
        return;
    }
    auto row = numbers(message.value(u"values"_s).toArray());
    if (message.value(u"raw"_s).toBool()) {
        // 原始抽样维度在独立进程中用双精度统计，避免 GPU 平方溢出并减少采样开销。
        const auto values = message.value(u"values"_s).toArray();
        if (values.isEmpty() || values.size() > 128) return;
        double sum = 0, peak = 0; int zero = 0, nonfinite = 0; QList<double> vector;
        for (const auto &entry : values) {
            const bool valid = entry.isDouble() && std::isfinite(entry.toDouble());
            const double value = valid ? entry.toDouble() : 0;
            if (!valid) ++nonfinite;
            vector.append(value); sum += value * value; peak = std::max(peak, std::abs(value));
            if (std::abs(value) < 1e-8) ++zero;
        }
        row = {std::sqrt(sum / values.size()), peak, double(zero) / values.size(), double(nonfinite)};
        row += vector;
    }
    if (row.size() < 5) return;
    const double logRms = std::log(std::max(1e-30, row[0]));
    const auto key = referenceKey(operation + u"/log_rms"_s, layer, phase, context); const auto ref = reference(key);
    const double center = median(ref), limit = std::max(std::log(8.), 6 * deviation(ref));
    const bool mature = ref.size() >= 32;
    const bool growth = mature && logRms > center + limit;
    const bool decay = mature && logRms < center - limit;
    evidence[u"rms"_s] = row[0]; evidence[u"peak"_s] = row[1]; evidence[u"near_zero"_s] = row[2];
    evidence[u"baseline_rms"_s] = std::exp(center); evidence[u"baseline_samples"_s] = ref.size();
    condition(id + u"/growth"_s, u"activation_growth"_s, growth, evidence);
    condition(id + u"/decay"_s, u"activation_decay"_s, decay, evidence);
    condition(id + u"/finite"_s, u"nonfinite"_s, row[3] > 0, evidence, 1);
    learn(key, logRms, growth || decay || row[3] > 0);
    if (operation == u"residual"_s && phase == u"decode"_s) {
        if (!windows.contains(id) && windows.size() >= 2048) { transportGap = true; return; }
        auto &window = windows[id]; const auto vector = row.mid(4);
        if (!window.vectors.isEmpty()) {
            const double similarity = cosine(vector, window.vectors.last());
            const auto refKey = referenceKey(u"hidden_cosine"_s, layer, phase, context); const auto base = reference(refKey);
            window.vectors.append(vector); while (window.vectors.size() > 8) window.vectors.removeFirst();
            // 小型 Gram 矩阵参与率近似有效维度，不调用全量奇异值分解。
            double trace = 0, squared = 0;
            for (int i = 0; i < window.vectors.size(); ++i) for (int j = 0; j < window.vectors.size(); ++j) {
                const double value = cosine(window.vectors[i], window.vectors[j]);
                squared += value * value; if (i == j) trace += value;
            }
            const double rank = squared > 0 ? trace * trace / squared : 0;
            const bool bad = base.size() >= 32 && window.vectors.size() >= 8 && similarity > .999 && rank < 1.1 && similarity > median(base) + std::max(.005, 6 * deviation(base));
            evidence[u"similarity"_s] = similarity; evidence[u"effective_dimensions"_s] = rank;
            condition(id + u"/representation"_s, u"representation_decay"_s, bad, evidence);
            learn(refKey, similarity, bad);
        } else window.vectors.append(vector);
    }
}

void AnomalyEngine::token(const QJsonObject &message, qint64 now) {
    // Token连续性、文本重复和延迟分别判断，缺口不参与连续证据拼接。
    const int step = message.value(u"step"_s).toInt();
    if (lastStep >= 0 && step != lastStep + 1) {
        transportGap = true; recentTokens.clear(); reasoning.clear(); answer.clear();
        for (auto it = gates.begin(); it != gates.end(); ++it) { it->bad = it->good = 0; }
    }
    lastStep = step; lastProgress = now;
    const bool resumed = waiting; waiting = false;
    recentTokens.append(message.value(u"id"_s).toInt()); while (recentTokens.size() > 256) recentTokens.removeFirst();
    bool repeating = false; int period = 0;
    for (int size = 1; size <= 32 && size * 4 <= recentTokens.size(); ++size) {
        bool same = true;
        for (int i = 0; i < size * 3; ++i)
            if (recentTokens[recentTokens.size() - 1 - i] != recentTokens[recentTokens.size() - 1 - i - size]) { same = false; break; }
        if (same) { repeating = true; period = size; break; }
    }
    condition(u"tokens/repetition"_s, u"repetition"_s, repeating && recentTokens.size() >= 16,
              {{u"period"_s, period}, {u"window_tokens"_s, recentTokens.size()}}, 3);
    const auto channel = message.value(u"channel"_s).toString();
    if (channel != u"control"_s) {
        auto &text = channel == u"reasoning"_s ? reasoning : answer;
        text += message.value(u"piece"_s).toString(); if (text.size() > 4096) text = text.right(4096);
        if (channel == u"reasoning"_s) ++reasoningTokens;
        if (step % 16 == 0 && text.size() >= 512) {
            const double similarity = shingles(text.right(256), text.mid(text.size() - 512, 256));
            condition(u"text/"_s + channel, u"rumination"_s, similarity > .85,
                      {{u"similarity"_s, similarity}, {u"approximate"_s, true}}, 2);
        }
    }
    condition(u"thinking/length"_s, u"overthinking"_s,
              channel == u"reasoning"_s && reasoningTokens >= 2048 && (repeating || now - started > 300000),
              {{u"thinking_tokens"_s, reasoningTokens}, {u"duration_ms"_s, double(now - started)}});
    const int maximum = metadata.value(u"max_tokens"_s).toInt();
    condition(u"length"_s, u"generation_limit"_s, (maximum >= 128 && step + 1 >= maximum * .95) ||
              (step >= 4096 && now - started > 300000),
              {{u"generated_tokens"_s, step + 1}, {u"maximum"_s, maximum}}, 1);
    const double elapsed = message.value(u"elapsed_ms"_s).toDouble();
    if (step > 0 && !resumed && !metadata.value(u"step_mode"_s).toBool() && elapsed > 0) {
        const auto key = referenceKey(u"token_latency"_s, -1, u"decode"_s, message.value(u"input_tokens"_s).toInt());
        const auto ref = reference(key); const double center = median(ref);
        const bool bad = ref.size() >= 32 && elapsed > std::max(center * 3, center + 6 * deviation(ref)) && elapsed > center + 20;
        condition(u"latency"_s, u"slow_generation"_s, bad,
                  {{u"elapsed_ms"_s, elapsed}, {u"baseline_ms"_s, center}});
        learn(key, elapsed, bad);
    }
}

void AnomalyEngine::finish(const QJsonObject &message, qint64 now) {
    // 结束后只保留证据；不会为已经结束的运行再询问是否继续。
    if (message.value(u"outcome"_s).toString() == u"completed"_s && !hadAlert && !transportGap) {
        for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
            if (!baselines.contains(it.key()) && baselines.size() >= 2048) break;
            auto &values = baselines[it.key()].values;
            for (double value : it->values) if (values.size() < 64) values.append(value);
        }
        saveReferences();
    }
    const bool incomplete = !message.value(u"thinking_complete"_s).toBool(true);
    emitEvent(u"anomaly.finished"_s, {{u"unfinished_thinking"_s, incomplete},
              {u"reason"_s, message.value(u"reason"_s)}, {u"dropped"_s, dropped}, {u"tensor_samples"_s, validTensorSamples}});
    status(now); active = false;
}

void AnomalyEngine::tick(qint64 now) {
    if (!active) return;
    // 用户暂停、单步等待及刚恢复时不累计停滞；监控提示本身从不改变等待状态。
    if (waiting) lastProgress = now;
    const bool noHeartbeat = !waiting && now - lastHeartbeat > (lastStep < 0 ? 120000 : 60000);
    const bool noProgress = !waiting && now - lastProgress > (lastStep < 0 ? 120000 : 30000);
    condition(u"heartbeat"_s, u"unresponsive"_s, noHeartbeat,
              {{u"silent_ms"_s, double(now - lastHeartbeat)}}, 2);
    condition(u"progress"_s, u"no_progress"_s, noProgress && !noHeartbeat,
              {{u"silent_ms"_s, double(now - lastProgress)}}, 2);
    if (now - lastStatus >= 1000) { status(now); lastStatus = now; }
}

void AnomalyEngine::status(qint64) {
    // 只报告当前模型配置的基线成熟度，同时明确暴露覆盖不足。
    int matured = 0; QSet<QString> keys;
    for (auto it = baselines.cbegin(); it != baselines.cend(); ++it) if (it.key().startsWith(profile + u"/"_s)) keys.insert(it.key());
    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) keys.insert(it.key());
    for (const auto &key : keys) if (reference(key).size() >= 32) ++matured;
    emitEvent(u"anomaly.status"_s, {{u"dropped"_s, dropped}, {u"sampled"_s, sampled}, {u"eligible"_s, eligible},
              {u"valid_tensor_samples"_s, validTensorSamples}, {u"baseline_metrics"_s, matured},
              {u"degraded"_s, transportGap}, {u"waiting"_s, waiting}});
}

void AnomalyEngine::loadReferences(const QString &project) {
    // 项目切换时加载有界统计文件；损坏或版本不兼容的文件不参与判断。
    const auto next = project.isEmpty() ? QString() : QDir(project).filePath(u"monitor"_s);
    if (next == directory) return;
    directory = next; baselines.clear();
    QFile file(QDir(directory).filePath(u"baselines.json"_s));
    if (directory.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return;
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value(u"schema"_s).toInt() != 1) return;
    const auto metrics = object.value(u"metrics"_s).toObject();
    for (auto it = metrics.begin(); it != metrics.end() && baselines.size() < 2048; ++it) {
        auto values = numbers(it.value().toArray()); if (values.size() > 64) values = values.mid(0, 64);
        baselines[it.key()].values = values;
    }
}

void AnomalyEngine::saveReferences() {
    // 一轮健康完成后原子保存统计参考，避免中断写出半份基线。
    if (directory.isEmpty()) return;
    QDir().mkpath(directory); QJsonObject metrics;
    for (auto it = baselines.cbegin(); it != baselines.cend(); ++it) {
        QJsonArray values; for (double value : it->values) values.append(value); metrics[it.key()] = values;
    }
    const auto data = QJsonDocument(QJsonObject{{u"schema"_s, 1}, {u"metrics"_s, metrics}}).toJson(QJsonDocument::Compact);
    if (data.size() > 4 * 1024 * 1024) return;
    QSaveFile file(QDir(directory).filePath(u"baselines.json"_s));
    if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size()) file.commit();
}
