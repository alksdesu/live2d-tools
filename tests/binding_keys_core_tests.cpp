#include "real_core_model_fixture.h"
#include "live2dstruct/ckeyformbindinggrid.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmocparameterstructurebuilder.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include <QCryptographicHash>
#include <QtEndian>

using namespace live2dstruct;
using Builder = CMocParameterStructureBuilder;

namespace {
QString group(Builder::Owner::Type type)
{
    return type == Builder::Owner::ArtMesh ? QStringLiteral("art_meshes")
        : type == Builder::Owner::Warp ? QStringLiteral("warp_deformers") : QStringLiteral("rotation_deformers");
}
QString formsGroup(Builder::Owner::Type type)
{
    return type == Builder::Owner::ArtMesh ? QStringLiteral("art_mesh_keyforms")
        : type == Builder::Owner::Warp ? QStringLiteral("warp_deformer_keyforms") : QStringLiteral("rotation_deformer_keyforms");
}
Builder::Owner owner(int type)
{
    return {Builder::Owner::Type(type), type == 0 ? QStringLiteral("Hair2") : type == 1 ? QStringLiteral("Warp10") : QStringLiteral("Root")};
}
QString axisId(int axis) { return QStringLiteral("ParamCoreGridAxis%1").arg(axis); }
Builder::BindingKey retained(float value, int old) { return {value, old, Builder::BindingKey::InterpolateOldAxis, -1}; }
Builder::BindingKey inserted(float value, int copy = -1)
{
    return {value, -1, copy < 0 ? Builder::BindingKey::InterpolateOldAxis : Builder::BindingKey::CopyOldSlice, copy};
}
QByteArray word(int value) { QByteArray result(4, '\0'); qToLittleEndian(qint32(value), result.data()); return result; }
QByteArray real(float value)
{
    quint32 bits; std::memcpy(&bits, &value, 4);
    QByteArray result(4, '\0'); qToLittleEndian(bits, result.data()); return result;
}
float realAt(const QByteArray& bytes, int offset)
{
    const auto bits = qFromLittleEndian<quint32>(bytes.constData() + offset);
    float value; std::memcpy(&value, &bits, 4); return value;
}

class RawColumns
{
public:
    explicit RawColumns(const QByteArray& source, const CSectionTable& table) : bytes(source)
    {
        for (const auto& item : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
            spans.insert(QString::fromLatin1(item.name), item);
    }
    bool has(const QString& name) const { return spans.contains(name) && spans.value(name).offset != 0; }
    QByteArray original(const QString& name) const
    {
        const auto span = spans.value(name); return bytes.mid(span.offset, span.count * span.width);
    }
    QByteArray& edit(const QString& name)
    {
        const auto span = spans.value(name);
        if (!state.columns.contains(span.slot)) state.columns.insert(span.slot, original(name));
        return state.columns[span.slot];
    }
    int integer(const QString& name, int row) const { const auto span = spans.value(name); return qFromLittleEndian<qint32>(bytes.constData() + span.offset + row * 4); }
    float number(const QString& name, int row) const { return realAt(bytes, spans.value(name).offset + row * 4); }
    void setInt(const QString& name, int row, int value) { edit(name).replace(row * 4, 4, word(value)); }
    void count(const QString& name, int value) { state.counts.insert(spans.value(name).countIndex, value); }
    int count(const QString& name) const { return int(spans.value(name).count); }
    QByteArray bytes;
    QMap<QString, CSectionValidation::ColumnSpan> spans;
    Builder::ColumnState state;
};

struct OwnerRows { int source = -1, first = -1, count = 0, vertices = 0; };
OwnerRows rows(const CSectionTable& table, const Builder::Owner& target)
{
    if (target.type == Builder::Owner::ArtMesh) {
        for (int index = 0; index < table.art_meshes.size(); ++index) {
            const auto* value = table.art_meshes[index];
            if (value->ids == target.id) return {index, value->keyform_sources_begin_indices, value->keyform_sources_counts, value->vertex_counts};
        }
    } else for (const auto* item : table.deformers) if (item->ids == target.id && item->types == int(target.type) - 1) {
        if (target.type == Builder::Owner::Warp) {
            const auto* value = table.warp_deformers[item->specific_sources_indices];
            return {item->specific_sources_indices, value->keyform_sources_begin_indices, value->keyform_sources_counts, value->vertex_counts};
        }
        const auto* value = table.rotation_deformers[item->specific_sources_indices];
        return {item->specific_sources_indices, value->keyform_sources_begin_indices, value->keyform_sources_counts, 0};
    }
    return {};
}

QSet<int> descendants(const CSectionTable& table, const Builder::Owner& target)
{
    QSet<int> result;
    int deformer = -1;
    for (int i = 0; i < table.deformers.size(); ++i) if (table.deformers[i]->ids == target.id) deformer = i;
    for (int index = 0; index < table.art_meshes.size(); ++index) {
        if (target.type == Builder::Owner::ArtMesh) { if (table.art_meshes[index]->ids == target.id) result.insert(index); continue; }
        int parent = table.art_meshes[index]->parent_deformer_indices;
        for (int depth = 0; parent >= 0 && parent < table.deformers.size() && depth < table.deformers.size(); ++depth) {
            if (parent == deformer) { result.insert(index); break; }
            parent = table.deformers[parent]->parent_deformer_indices;
        }
    }
    return result;
}

QVector<float> defaults(csmModel* model)
{
    const auto* values = csmGetParameterDefaultValues(model);
    return QVector<float>(values, values + csmGetParameterCount(model));
}
int parameter(csmModel* model, const QString& id)
{
    for (int index = 0; index < csmGetParameterCount(model); ++index)
        if (QString::fromUtf8(csmGetParameterIds(model)[index]) == id) return index;
    return -1;
}

bool colored(const QByteArray& source, const Builder::Owner& target, QByteArray& result, QString* error)
{
    auto reader = CReader::FromBytes(source); CSectionTable table(reader);
    if (!table.IsValid()) return false;
    const auto ownerRows = rows(table, target); RawColumns columns(source, table);
    const QString ownerColor = group(target.type) + ".keyform_color_sources_begin_indices";
    if (!columns.has(ownerColor)) { result = source; return true; }
    const int first = qMax(columns.count("keyform_multiply_colors.r"), columns.count("keyform_screen_colors.r"));
    for (const auto& channel : {QStringLiteral("multiply"), QStringLiteral("screen")}) {
        const QString pool = "keyform_" + channel + "_colors";
        for (int component = 0; component < 3; ++component) {
            const QString field = pool + '.' + QStringLiteral("rgb")[component];
            auto& data = columns.edit(field); data.append(QByteArray(first * 4 - data.size(), '\0'));
            for (int local = 0; local < ownerRows.count; ++local)
                data.append(real(float((local % 11) + component + 1) / 32.0f));
        }
        columns.count(pool + ".r", first + ownerRows.count);
        const QString indices = formsGroup(target.type) + '.' + channel + "_color_sources_indices";
        if (columns.has(indices)) for (int local = 0; local < ownerRows.count; ++local)
            columns.setInt(indices, ownerRows.first + local, first + local);
    }
    columns.setInt(ownerColor, ownerRows.source, first);
    return Builder::BuildColumnState(source, columns.state, result, error);
}

bool fixture(int type, bool reflection, QByteArray& result, QString* error)
{
    const QString name = type == 0 ? QStringLiteral("Mark") : QStringLiteral("Natori");
    QByteArray bytes = read(QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3");
    auto reader = CReader::FromBytes(bytes); CSectionTable original(reader);
    CKeyformBindingGrid grid;
    if (!original.IsValid() || !grid.Build(original, error) || !grid.Axes(CKeyformBindingGrid::ObjectType(type), owner(type).id).isEmpty()) return false;
    if (rows(original, owner(type)).count != 1) return false;
    for (int axis = 0; axis < 3; ++axis) {
        Builder::CreateRequest create; create.parameter.id = axisId(axis);
        create.parameter.minimum = -2; create.parameter.maximum = 2;
        create.bindings.append({owner(type), {-1, 0, 1}});
        Builder::Result created;
        if (!Builder::Create(bytes, create, created, error)) return false;
        bytes = created.bytes;
    }
    CModelDocument document;
    if (!document.LoadBytes(bytes, name + ".moc3", error)) return false;
    const auto opacity = [](int local) { return .25f + .125f * (local % 3) + .0625f * ((local / 3) % 3) + .03125f * (local / 9); };
    const auto positions = [](QVector<float> values, int local) {
        const int a = local % 3 - 1, b = (local / 3) % 3 - 1, c = local / 9 - 1;
        for (int index = 0; index < values.size(); ++index)
            values[index] += (index % 2 ? -.0075f : .0125f) * (a + 2 * b + 4 * c) * (1 + index % 3);
        return values;
    };
    if (type == 0) {
        const auto found = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(), [](const auto& value) { return value.id == "Hair2"; });
        if (found == document.Drawables().cend()) return false;
        const auto forms = found->keyforms;
        for (int local = 0; local < forms.size(); ++local)
            if (!document.SetDrawableKeyformPositions(owner(type).id, local, positions(forms[local].positions, local))) return false;
    } else if (type == 1) {
        auto forms = document.DeformerWarpKeyforms(owner(type).id);
        for (int local = 0; local < forms.size(); ++local) { forms[local].opacity = opacity(local); forms[local].positions = positions(forms[local].positions, local); }
        if (!document.SetDeformerWarpKeyforms(owner(type).id, forms)) return false;
    } else {
        auto forms = document.DeformerRotationKeyforms(owner(type).id);
        for (int local = 0; local < forms.size(); ++local) {
            const int a = local % 3 - 1, b = (local / 3) % 3 - 1, c = local / 9 - 1;
            auto& form = forms[local];
            form.opacity = opacity(local); form.angle += a * 12 + b * 6 + c * 3;
            form.originX += a * .0125f + b * .025f; form.originY += c * .025f;
            form.scale *= a + b * .125f + c * .25f;
            if (reflection) { form.reflectX = (local % 3) == 1; form.reflectY = ((local / 3) % 3) == 1; }
        }
        if (!document.SetDeformerRotationKeyforms(owner(type).id, forms)) return false;
    }
    if (!document.BuildPreviewMoc(bytes, error)) return false;
    if (type == 0) {
        auto parsed = CReader::FromBytes(bytes); CSectionTable table(parsed); const auto target = rows(table, owner(type));
        for (int local = 0; local < target.count; ++local) {
            bytes.replace(table.sectionOffsetTable->art_mesh_keyforms->opacities + (target.first + local) * 4, 4, real(opacity(local)));
            const int offset = table.sectionOffsetTable->art_mesh_keyforms->draw_orders + (target.first + local) * 4;
            bytes.replace(offset, 4, real(realAt(bytes, offset) + (local % 3) * 8 + ((local / 3) % 3) * 4 + (local / 9) * 2));
        }
    }
    return colored(bytes, owner(type), result, error);
}

using Terms = QVector<QPair<int, double>>;
struct Operation { QVector<Builder::BindingKey> keys; QVector<Terms> slices; };
Operation operation(const QString& name)
{
    if (name == "delete") return {{retained(-1, 0), retained(1, 2)}, {{{0, 1}}, {{2, 1}}}};
    if (name == "move") return {{retained(-1, 0), retained(.375f, 1), retained(1, 2)}, {{{0, 1}}, {{1, 1}}, {{2, 1}}}};
    if (name == "copy") return {{retained(-1, 0), retained(0, 1), inserted(.5f, 0), retained(1, 2)}, {{{0, 1}}, {{1, 1}}, {{0, 1}}, {{2, 1}}}};
    if (name == "extend-copy") return {{retained(-1, 0), retained(0, 1), retained(1, 2), inserted(1.5f, 2)}, {{{0, 1}}, {{1, 1}}, {{2, 1}}, {{2, 1}}}};
    return {{retained(-1, 0), inserted(-.5f), retained(0, 1), retained(1, 2)}, {{{0, 1}}, {{0, .5}, {1, .5}}, {{1, 1}}, {{2, 1}}}};
}

bool oracle(const QByteArray& bytes, const Builder::EditBindingKeysRequest& request, const Operation& op,
            QByteArray& expected, QString* error)
{
    auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); CKeyformBindingGrid grid;
    if (!table.IsValid() || !grid.Build(table, error)) return false;
    const auto axes = grid.Axes(CKeyformBindingGrid::ObjectType(request.owner.type), request.owner.id);
    if (request.axisIndex < 0 || request.axisIndex >= axes.size()) return false;
    const auto target = rows(table, request.owner); RawColumns columns(bytes, table);
    const auto prefix = formsGroup(request.owner.type);
    const int first = columns.count(prefix + ".opacities");
    const int oldAxisCount = axes[request.axisIndex].keys.size();
    const int total = target.count / oldAxisCount * op.keys.size();
    int stride = 1;
    for (int axis = 0; axis < request.axisIndex; ++axis) stride *= axes[axis].keys.size();
    const int binding = axes[request.axisIndex].bindingIndex;
    const int keyFirst = columns.count("keys.values");
    for (const auto& key : op.keys) columns.edit("keys.values").append(real(key.value));
    columns.count("keys.values", keyFirst + op.keys.size());
    columns.setInt("parameter_bindings.keys_sources_begin_indices", binding, keyFirst);
    columns.setInt("parameter_bindings.keys_sources_counts", binding, op.keys.size());
    int parameterIndex = -1;
    for (int index = 0; index < table.parameters.size(); ++index) if (table.parameters[index]->ids == request.parameterId) parameterIndex = index;
    if (parameterIndex < 0) return false;
    if (columns.has("parameters.keys_sources_begin_indices")) {
        columns.setInt("parameters.keys_sources_begin_indices", parameterIndex, keyFirst);
        columns.setInt("parameters.keys_sources_counts", parameterIndex, op.keys.size());
    }
    const auto ownerPrefix = group(request.owner.type);
    const auto ownerColor = ownerPrefix + ".keyform_color_sources_begin_indices";
    const bool color = columns.has(ownerColor) && columns.integer(ownerColor, target.source) >= 0;
    const int colorFirst = qMax(columns.count("keyform_multiply_colors.r"), columns.count("keyform_screen_colors.r"));
    if (color) {
        for (const auto& channel : {QStringLiteral("multiply"), QStringLiteral("screen")})
            for (const QChar component : QStringLiteral("rgb")) {
                auto& data = columns.edit("keyform_" + channel + "_colors." + component);
                data.append(QByteArray(colorFirst * 4 - data.size(), '\0'));
            }
    }
    for (int local = 0; local < total; ++local) {
        const int digit = (local / stride) % op.keys.size();
        const int inner = local % stride, outer = local / (stride * op.keys.size());
        Terms terms;
        for (const auto& term : op.slices[digit]) terms.append({target.first + inner + term.first * stride + outer * stride * oldAxisCount, term.second});
        if (color) for (const auto& channel : {QStringLiteral("multiply"), QStringLiteral("screen")}) {
            const auto indexField = prefix + '.' + channel + "_color_sources_indices";
            for (const QChar component : QStringLiteral("rgb")) {
                const auto field = "keyform_" + channel + "_colors." + component;
                double value = 0;
                int originalColor = -1;
                for (const auto& term : terms) {
                    const int source = columns.has(indexField) ? columns.integer(indexField, term.first)
                        : columns.integer(ownerColor, target.source) + term.first - target.first;
                    originalColor = source;
                    value += columns.number(field, source) * term.second;
                }
                columns.edit(field).append(terms.size() == 1 ? columns.original(field).mid(originalColor * 4, 4) : real(float(value)));
            }
        }
        for (auto it = columns.spans.cbegin(); it != columns.spans.cend(); ++it) {
            if (!it.key().startsWith(prefix + '.') || !it->offset) continue;
            const auto field = it.key().mid(prefix.size() + 1);
            QByteArray value;
            if (field.endsWith("color_sources_indices") && color) value = word(colorFirst + local);
            else if (field == "keyform_position_sources_begin_indices") {
                value = word(columns.edit("keyform_positions.xys").size() / 4);
                for (int component = 0; component < target.vertices * 2; ++component) {
                    if (terms.size() == 1) {
                        const int source = columns.integer(it.key(), terms.first().first) + component;
                        columns.edit("keyform_positions.xys").append(columns.original("keyform_positions.xys").mid(source * 4, 4));
                        continue;
                    }
                    double position = 0;
                    for (const auto& term : terms) position += columns.number("keyform_positions.xys", columns.integer(it.key(), term.first) + component) * term.second;
                    columns.edit("keyform_positions.xys").append(real(float(position)));
                }
            } else if (field.startsWith("is_reflect_") || terms.size() == 1) value = columns.original(it.key()).mid(terms.first().first * 4, 4);
            else {
                double number = 0;
                for (const auto& term : terms) number += columns.number(it.key(), term.first) * term.second;
                value = real(float(number));
            }
            columns.edit(it.key()).append(value);
        }
    }
    columns.count(prefix + ".opacities", first + total);
    if (target.vertices) columns.count("keyform_positions.xys", columns.edit("keyform_positions.xys").size() / 4);
    columns.setInt(ownerPrefix + ".keyform_sources_begin_indices", target.source, first);
    columns.setInt(ownerPrefix + ".keyform_sources_counts", target.source, total);
    if (color) {
        columns.setInt(ownerColor, target.source, colorFirst);
        columns.count("keyform_multiply_colors.r", colorFirst + total);
        columns.count("keyform_screen_colors.r", colorFirst + total);
    }
    return Builder::BuildColumnState(bytes, columns.state, expected, error);
}
}

class BindingKeysCoreTests : public QObject
{
    Q_OBJECT
    QMap<int, QByteArray> m_fixtures;
    QByteArray m_reflection;

    void protectedPrefixes(const QByteArray& before, const QByteArray& after)
    {
        auto aReader = CReader::FromBytes(before), bReader = CReader::FromBytes(after);
        CSectionTable a(aReader), b(bReader); QVERIFY(a.IsValid()); QVERIFY(b.IsValid());
        RawColumns original(before, a), candidate(after, b);
        for (auto it = original.spans.cbegin(); it != original.spans.cend(); ++it) {
            if (!it->offset || !it->count) continue;
            if (!it.key().startsWith("blend_shape") && !it.key().startsWith("keyform_positions.")
                && !it.key().startsWith("keyform_multiply_colors.") && !it.key().startsWith("keyform_screen_colors.")
                && !it.key().startsWith("art_mesh_keyforms.") && !it.key().startsWith("warp_deformer_keyforms.")
                && !it.key().startsWith("rotation_deformer_keyforms.")) continue;
            const auto old = original.original(it.key()), next = candidate.original(it.key());
            QVERIFY2(next.startsWith(old), qPrintable(it.key()));
        }
    }

    void compare(const QByteArray& source, const Builder::EditBindingKeysRequest& request, const Operation& op,
                 bool preserveFunction)
    {
        const auto immutable = source;
        const auto sourceDigest = QCryptographicHash::hash(source, QCryptographicHash::Sha256);
        Builder::Result actual; QByteArray expected; QString error;
        QVERIFY2(Builder::EditBindingKeys(source, request, actual, &error), qPrintable(error));
        QVERIFY2(oracle(source, request, op, expected, &error), qPrintable(error));
        QCOMPARE(source, immutable);
        QCOMPARE(QCryptographicHash::hash(source, QCryptographicHash::Sha256), sourceDigest);
        protectedPrefixes(source, actual.bytes);
        CoreModel before, after, reference;
        QVERIFY(before.load(source)); QVERIFY(after.load(actual.bytes)); QVERIFY(reference.load(expected));
        auto reader = CReader::FromBytes(source); CSectionTable table(reader);
        auto afterReader = CReader::FromBytes(actual.bytes); CSectionTable afterTable(afterReader); QVERIFY(afterTable.IsValid());
        CKeyformBindingGrid oldGrid, newGrid; QVERIFY(oldGrid.Build(table)); QVERIFY(newGrid.Build(afterTable));
        const auto oldAxes = oldGrid.Axes(CKeyformBindingGrid::ObjectType(request.owner.type), request.owner.id);
        const auto nextAxes = newGrid.Axes(CKeyformBindingGrid::ObjectType(request.owner.type), request.owner.id);
        QCOMPARE(nextAxes.size(), oldAxes.size());
        QVector<float> keys; for (const auto& key : op.keys) keys.append(key.value);
        for (int index = 0; index < oldAxes.size(); ++index) {
            QCOMPARE(nextAxes[index].parameterId, oldAxes[index].parameterId);
            QCOMPARE(nextAxes[index].keys, index == request.axisIndex ? keys : oldAxes[index].keys);
        }
        QCOMPARE(rows(afterTable, request.owner).count, rows(table, request.owner).count / request.expectedKeys.size() * op.keys.size());
        const auto scope = descendants(table, request.owner); QVERIFY(!scope.isEmpty());
        auto baseline = defaults(before.value);
        const int axis = parameter(before.value, request.parameterId); QVERIFY(axis >= 0);
        QVector<float> samples;
        for (int index = 0; index < op.keys.size(); ++index) {
            samples.append(op.keys[index].value);
            if (index) samples.append((op.keys[index - 1].value + op.keys[index].value) * .5f);
        }
        for (int variant = 0; variant < 4; ++variant) {
            auto values = baseline;
            for (int other = 0; other < 3; ++other) {
                const int index = parameter(before.value, axisId(other));
                if (index >= 0 && index != axis) values[index] = variant == 0 ? -1.0f : variant == 1 ? 0.0f : variant == 2 ? .3125f : 1.0f;
            }
            for (float value : samples) {
                values[axis] = value;
                after.pose(values); reference.pose(values);
                const auto actualFrame = frame(after.value), expectedFrame = frame(reference.value);
                QVERIFY2(changed(actualFrame, expectedFrame, scope).isEmpty(), qPrintable(difference(actualFrame, expectedFrame, scope)));
                if (preserveFunction) {
                    before.pose(values);
                    QVERIFY2(changed(frame(before.value), actualFrame, scope).isEmpty(), qPrintable(difference(frame(before.value), actualFrame, scope)));
                }
            }
            for (int key = 0; key < op.keys.size(); ++key) if (op.slices[key].size() == 1) {
                auto previous = values; previous[axis] = request.expectedKeys[op.slices[key].first().first];
                values[axis] = op.keys[key].value;
                before.pose(previous); after.pose(values);
                QVERIFY2(changed(frame(before.value), frame(after.value), scope).isEmpty(), qPrintable(difference(frame(before.value), frame(after.value), scope)));
            }
        }
        QVERIFY(before.intact() && after.intact() && reference.intact());
    }

private slots:
    void initTestCase()
    {
        QString error;
        for (int type = 0; type < 3; ++type) {
            QByteArray bytes; QVERIFY2(fixture(type, false, bytes, &error), qPrintable(error));
            CoreModel accepted; QVERIFY(accepted.load(bytes));
            auto a = defaults(accepted.value), b = a;
            for (int axis = 0; axis < 3; ++axis) { a[parameter(accepted.value, axisId(axis))] = -1; b[parameter(accepted.value, axisId(axis))] = 1; }
            accepted.pose(a); const auto first = frame(accepted.value); accepted.pose(b);
            QVERIFY(!changed(first, frame(accepted.value)).isEmpty());
            m_fixtures.insert(type, bytes);
        }
        QVERIFY2(fixture(2, true, m_reflection, &error), qPrintable(error));
    }
    void axisChangesMatchIndependentRawOracle_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<int>("axis"); QTest::addColumn<QString>("operationName");
        for (int type = 0; type < 3; ++type) for (int axis = 0; axis < 3; ++axis)
            for (const QString& operation : {QString("insert"), QString("copy"), QString("move"), QString("delete"), QString("extend-copy")})
                QTest::newRow(qPrintable(QString("type%1-axis%2-%3").arg(type).arg(axis).arg(operation))) << type << axis << operation;
    }
    void axisChangesMatchIndependentRawOracle()
    {
        QFETCH(int, type); QFETCH(int, axis); QFETCH(QString, operationName);
        const auto op = operation(operationName);
        compare(m_fixtures.value(type), {owner(type), axis, axisId(axis), {-1, 0, 1}, op.keys}, op,
                operationName == "insert");
    }
    void signedScaleAndReflection_data()
    {
        QTest::addColumn<int>("axis"); QTest::addColumn<QString>("operationName");
        for (int axis = 0; axis < 3; ++axis) for (const QString& operation : {QString("insert"), QString("move"), QString("delete")})
            QTest::newRow(qPrintable(QString("axis%1-%2").arg(axis).arg(operation))) << axis << operation;
    }
    void signedScaleAndReflection()
    {
        QFETCH(int, axis); QFETCH(QString, operationName);
        const auto op = operation(operationName);
        compare(m_reflection, {owner(2), axis, axisId(axis), {-1, 0, 1}, op.keys}, op, false);
    }

    void outsideOldKeysRejectImplicitInterpolationAndFreeze_data()
    {
        QTest::addColumn<int>("type"); QTest::addColumn<int>("axis"); QTest::addColumn<float>("value");
        for (int type = 0; type < 3; ++type) for (int axis = 0; axis < 3; ++axis) for (float value : {-1.5f, 1.5f})
            QTest::newRow(qPrintable(QString("type%1-axis%2-%3").arg(type).arg(axis).arg(value))) << type << axis << value;
    }
    void outsideOldKeysRejectImplicitInterpolationAndFreeze()
    {
        QFETCH(int, type); QFETCH(int, axis); QFETCH(float, value);
        const auto bytes = m_fixtures.value(type), digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        CoreModel live; QVERIFY(live.load(bytes)); live.pose(defaults(live.value)); const auto unchanged = frame(live.value);
        Builder::EditBindingKeysRequest request{owner(type), axis, axisId(axis), {-1, 0, 1}, {retained(-1, 0), retained(0, 1), retained(1, 2)}};
        if (value < 0) request.keys.prepend(inserted(value)); else request.keys.append(inserted(value));
        Builder::Result result; result.bytes = "untouched"; result.oldParameterToNew = {91}; QString error;
        QVERIFY(!Builder::EditBindingKeys(bytes, request, result, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(result.bytes, QByteArray("untouched")); QCOMPARE(result.oldParameterToNew, QVector<int>({91}));
        Builder::DeleteImpact impact;
        if (Builder::AnalyzeDelete(bytes, {axisId(axis), value}, impact, &error)) QVERIFY(!impact.blockingReasons.isEmpty());
        else QVERIFY(!error.isEmpty());
        QVERIFY(!Builder::Delete(bytes, {axisId(axis), value}, result, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(result.bytes, QByteArray("untouched")); QCOMPARE(result.oldParameterToNew, QVector<int>({91}));
        QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), digest);
        QCOMPARE(frame(live.value).definition, unchanged.definition); QCOMPARE(frame(live.value).values, unchanged.values);
        QVERIFY(live.intact());
    }

    void explicitCopyBelowOldKeysHasAnIndependentOracle()
    {
        const Operation op{{inserted(-1.5f, 0), retained(-1, 0), retained(0, 1), retained(1, 2)},
                           {{{0, 1}}, {{0, 1}}, {{1, 1}}, {{2, 1}}}};
        compare(m_fixtures.value(0), {owner(0), 1, axisId(1), {-1, 0, 1}, op.keys}, op, false);
    }

    void maoOrdinaryAxisPreservesBlendShapesAndColors()
    {
        const QString path = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (path.isEmpty()) QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to the official Mao MOC to run the BlendShape/color fixture.");
        const QByteArray original = read(path); QVERIFY(!original.isEmpty());
        auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        QVERIFY(!table.blend_shapes_art_meshes.isEmpty());
        int meshIndex = -1;
        for (const auto* shape : table.blend_shapes_art_meshes) {
            const auto* mesh = table.art_meshes[shape->target_indices];
            if (!mesh->vertex_counts || !mesh->keyform_sources_counts) continue;
            if (meshIndex < 0 || mesh->keyform_sources_counts < table.art_meshes[meshIndex]->keyform_sources_counts)
                meshIndex = shape->target_indices;
        }
        QVERIFY(meshIndex >= 0);
        const Builder::Owner target{Builder::Owner::ArtMesh, table.art_meshes[meshIndex]->ids};
        const QString parameterId = QStringLiteral("ParamCoreMaoOrdinaryAxis");
        Builder::CreateRequest create; create.parameter.id = parameterId;
        create.bindings.append({target, {-1, 0, 1}});
        Builder::Result created; QString error;
        QVERIFY2(Builder::Create(original, create, created, &error), qPrintable(error));
        CModelDocument document; QVERIFY2(document.LoadBytes(created.bytes, path, &error), qPrintable(error));
        const int oldCount = table.art_meshes[meshIndex]->keyform_sources_counts;
        for (int local = 0; local < oldCount * 3; ++local) {
            auto positions = document.DrawableKeyformPositions(target.id, local);
            for (int component = 0; component < positions.size(); ++component)
                positions[component] += .00125f * (local / oldCount - 1) * (component % 2 ? -1 : 1);
            QVERIFY(document.SetDrawableKeyformPositions(target.id, local, positions));
        }
        QByteArray source, coloredSource;
        QVERIFY2(document.BuildPreviewMoc(source, &error), qPrintable(error));
        QVERIFY2(colored(source, target, coloredSource, &error), qPrintable(error)); source = coloredSource;
        auto sourceReader = CReader::FromBytes(source); CSectionTable sourceTable(sourceReader); QVERIFY(sourceTable.IsValid());
        CKeyformBindingGrid grid; QVERIFY(grid.Build(sourceTable));
        const auto axes = grid.Axes(CKeyformBindingGrid::ArtMesh, target.id);
        QVERIFY(!axes.isEmpty()); QCOMPARE(axes.last().parameterId, parameterId);
        RawColumns sourceColumns(source, sourceTable);
        QVERIFY(sourceColumns.has("art_meshes.keyform_color_sources_begin_indices"));
        QVERIFY(sourceColumns.count("keyform_multiply_colors.r") > 0);
        QVERIFY(sourceColumns.count("keyform_screen_colors.r") > 0);
        qInfo().noquote() << QStringLiteral("Mao BS/color owner %1, original forms %2, normal axes %3").arg(target.id).arg(oldCount).arg(axes.size());
        const auto op = operation("insert");
        Builder::EditBindingKeysRequest request{target, int(axes.size()) - 1, parameterId, {-1, 0, 1}, op.keys};
        Builder::Result actual; QByteArray expected;
        QVERIFY2(Builder::EditBindingKeys(source, request, actual, &error), qPrintable(error));
        QVERIFY2(oracle(source, request, op, expected, &error), qPrintable(error));
        protectedPrefixes(source, actual.bytes);
        CoreModel baseline, candidate, reference;
        QVERIFY(baseline.load(source)); QVERIFY(candidate.load(actual.bytes)); QVERIFY(reference.load(expected));
        const auto scope = descendants(sourceTable, target); QCOMPARE(scope.size(), 1);
        const int edited = parameter(baseline.value, parameterId); QVERIFY(edited >= 0);
        bool observedBlendShape = false, observedColor = false;
        Frame first;
        for (int weight = 0; weight < 3; ++weight) {
            auto values = defaults(baseline.value);
            for (int index = 0; index < values.size(); ++index) if (csmGetParameterTypes(baseline.value)[index] == 1)
                values[index] = csmGetParameterMinimumValues(baseline.value)[index] * (1 - .5f * weight)
                    + csmGetParameterMaximumValues(baseline.value)[index] * (.5f * weight);
            for (float key : {-1.0f, -.75f, -.5f, 0.0f, .625f, 1.0f}) {
                values[edited] = key;
                baseline.pose(values); candidate.pose(values); reference.pose(values);
                const auto before = frame(baseline.value), after = frame(candidate.value), rawExpected = frame(reference.value);
                QVERIFY2(changed(before, after, scope).isEmpty(), qPrintable(difference(before, after, scope)));
                QVERIFY2(changed(after, rawExpected, scope).isEmpty(), qPrintable(difference(after, rawExpected, scope)));
                if (weight == 0 && key == -1) first = before;
                if (weight > 0 && key == -1) observedBlendShape |= first.values[meshIndex] != before.values[meshIndex];
                if (weight == 0 && key == 1) {
                    const int drawable = *scope.cbegin();
                    observedColor = first.values[drawable].mid(first.positionFields[drawable] + 1)
                        != before.values[drawable].mid(before.positionFields[drawable] + 1);
                }
            }
        }
        QVERIFY(observedBlendShape); QVERIFY(observedColor);
        QVERIFY(baseline.intact() && candidate.intact() && reference.intact());
        QCOMPARE(original, read(path));
    }
};

QTEST_APPLESS_MAIN(BindingKeysCoreTests)
#include "binding_keys_core_tests.moc"
