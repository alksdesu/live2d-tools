#include "core/Live2DCubismCore.h"
#include "live2dstruct/cmocpreviewbuilder.h"
#include "live2dstruct/cmodeldocument.h"

#include <QCryptographicHash>
#include <QFile>
#include <QtEndian>
#include <QtTest>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace live2dstruct;

namespace {
struct AlignedMemory
{
    std::vector<unsigned char> storage;
    unsigned char* address = nullptr;
    size_t size = 0;

    void Allocate(size_t length, size_t alignment)
    {
        storage.assign(length + alignment + 128, 0xa5);
        const auto raw = reinterpret_cast<std::uintptr_t>(storage.data());
        address = reinterpret_cast<unsigned char*>((raw + alignment - 1) & ~(alignment - 1));
        size = length;
        std::memset(address, 0, length);
    }

    bool GuardsIntact() const
    {
        return address && std::all_of(storage.data(), static_cast<const unsigned char*>(address),
                                     [](unsigned char byte) { return byte == 0xa5; }) &&
            std::all_of(static_cast<const unsigned char*>(address) + size, storage.data() + storage.size(),
                        [](unsigned char byte) { return byte == 0xa5; });
    }
};

class CoreModel
{
public:
    CoreModel() = default;
    CoreModel(const CoreModel&) = delete;
    CoreModel& operator=(const CoreModel&) = delete;

    bool Load(const QByteArray& bytes, QString& error)
    {
        if (bytes.isEmpty() || quint64(bytes.size()) > std::numeric_limits<unsigned int>::max()) {
            error = QStringLiteral("Invalid input size");
            return false;
        }
        mocMemory.Allocate(size_t(bytes.size()), csmAlignofMoc);
        std::memcpy(mocMemory.address, bytes.constData(), size_t(bytes.size()));
        if (csmHasMocConsistency(mocMemory.address, unsigned(bytes.size())) != 1) {
            error = QStringLiteral("Core consistency rejected the candidate (%1 bytes)").arg(bytes.size());
            return false;
        }
        std::memcpy(mocMemory.address, bytes.constData(), size_t(bytes.size()));
        auto* moc = csmReviveMocInPlace(mocMemory.address, unsigned(bytes.size()));
        if (!moc) { error = QStringLiteral("Core revive failed"); return false; }
        const unsigned size = csmGetSizeofModel(moc);
        if (!size || size > 512u * 1024u * 1024u) {
            error = QStringLiteral("Core returned an invalid model size");
            return false;
        }
        modelMemory.Allocate(size, csmAlignofModel);
        model = csmInitializeModelInPlace(moc, modelMemory.address, size);
        if (!model) { error = QStringLiteral("Core model initialization failed"); return false; }
        error.clear();
        return GuardsIntact();
    }

    bool GuardsIntact() const { return mocMemory.GuardsIntact() && modelMemory.GuardsIntact(); }
    csmModel* Get() const { return model; }

    void Apply(const QVector<float>& pose)
    {
        Q_ASSERT(pose.size() == csmGetParameterCount(model));
        std::copy(pose.cbegin(), pose.cend(), csmGetParameterValues(model));
        csmResetDrawableDynamicFlags(model);
        csmUpdateModel(model);
    }

private:
    AlignedMemory mocMemory;
    AlignedMemory modelMemory;
    csmModel* model = nullptr;
};

QByteArray Read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void Models()
{
    QTest::addColumn<QString>("path");
    for (const char* name : {"Mark", "Natori", "Rice", "Wanko"})
        QTest::newRow(name) << QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3";
    const QString extra = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
    if (!extra.isEmpty()) QTest::newRow("external-moc") << extra;
}

int Word(const QByteArray& bytes, int address) { return qFromLittleEndian<qint32>(bytes.constData() + address); }
int Cell(const QByteArray& bytes, int slot, int row) { return Word(bytes, Word(bytes, slot) + row * 4); }
void Put(QByteArray& bytes, int address, int value) { qToLittleEndian(qint32(value), bytes.data() + address); }
void SetCell(QByteArray& bytes, int slot, int row, int value) { Put(bytes, Word(bytes, slot) + row * 4, value); }

void PutFloat(QByteArray& bytes, int address, float value)
{
    quint32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    qToLittleEndian(bits, bytes.data() + address);
}

template<class T> QByteArray Raw(const T* data, int count)
{
    return count ? QByteArray(reinterpret_cast<const char*>(data), qsizetype(count) * sizeof(T)) : QByteArray();
}

struct DrawableState
{
    QString id;
    QByteArray positions;
    QByteArray uvs;
    QByteArray indices;
    QVector<int> masks;
    QByteArray scalar;
};

struct State
{
    QStringList parameterIds;
    QStringList partIds;
    QByteArray parameterTypes;
    QVector<QByteArray> parameterKeys;
    QVector<float> parameterMinimums;
    QVector<float> parameterMaximums;
    QVector<float> parameterDefaults;
    QVector<float> parameterValues;
    QByteArray partParents;
    QByteArray partOpacities;
    QByteArray canvas;
    QVector<DrawableState> drawables;
};

bool Capture(CoreModel& owner, State& result, QString& error)
{
    auto* model = owner.Get();
    const auto fail = [&](const char* message) { error = QString::fromLatin1(message); return false; };
    const int parameters = csmGetParameterCount(model), parts = csmGetPartCount(model), drawables = csmGetDrawableCount(model);
    if (parameters < 0 || parts < 0 || drawables <= 0) return fail("Invalid Core counts");
    State state;
    const auto parameterIds = csmGetParameterIds(model);
    const auto parameterTypes = csmGetParameterTypes(model);
    const auto keyCounts = csmGetParameterKeyCounts(model);
    const auto keys = csmGetParameterKeyValues(model);
    const auto minimums = csmGetParameterMinimumValues(model), maximums = csmGetParameterMaximumValues(model);
    const auto defaults = csmGetParameterDefaultValues(model);
    const auto values = csmGetParameterValues(model);
    if (parameters && (!parameterIds || !parameterTypes || !keyCounts || !keys || !minimums || !maximums || !defaults || !values))
        return fail("Missing parameter arrays");
    state.parameterTypes = Raw(parameterTypes, parameters);
    for (int index = 0; index < parameters; ++index) {
        if (!parameterIds[index] || keyCounts[index] < 0 || (keyCounts[index] && !keys[index])) return fail("Invalid parameter definition");
        if (!std::isfinite(minimums[index]) || !std::isfinite(maximums[index]) || !std::isfinite(defaults[index]) ||
            !std::isfinite(values[index]) || minimums[index] > maximums[index]) return fail("Invalid parameter values");
        state.parameterIds.append(QString::fromUtf8(parameterIds[index]));
        state.parameterKeys.append(Raw(keys[index], keyCounts[index]));
        state.parameterMinimums.append(minimums[index]);
        state.parameterMaximums.append(maximums[index]);
        state.parameterDefaults.append(defaults[index]);
        state.parameterValues.append(values[index]);
    }
    const auto partIds = csmGetPartIds(model);
    const auto parents = csmGetPartParentPartIndices(model);
    const auto opacities = csmGetPartOpacities(model);
    if (parts && (!partIds || !parents || !opacities)) return fail("Missing part arrays");
    for (int index = 0; index < parts; ++index) {
        if (!partIds[index] || !std::isfinite(opacities[index])) return fail("Invalid part state");
        state.partIds.append(QString::fromUtf8(partIds[index]));
    }
    state.partParents = Raw(parents, parts);
    state.partOpacities = Raw(opacities, parts);
    csmVector2 canvas{}, origin{};
    float ppu = 0;
    csmReadCanvasInfo(model, &canvas, &origin, &ppu);
    const std::array<float, 5> canvasValues{canvas.X, canvas.Y, origin.X, origin.Y, ppu};
    for (float value : canvasValues) if (!std::isfinite(value)) return fail("Invalid canvas");
    state.canvas = Raw(canvasValues.data(), int(canvasValues.size()));
    const auto ids = csmGetDrawableIds(model);
    const auto vertices = csmGetDrawableVertexCounts(model);
    const auto positions = csmGetDrawableVertexPositions(model);
    const auto uvs = csmGetDrawableVertexUvs(model);
    const auto indices = csmGetDrawableIndices(model);
    const auto indexCounts = csmGetDrawableIndexCounts(model);
    const auto masks = csmGetDrawableMasks(model);
    const auto maskCounts = csmGetDrawableMaskCounts(model);
    const auto flags = csmGetDrawableConstantFlags(model);
    const auto dynamic = csmGetDrawableDynamicFlags(model);
    const auto texture = csmGetDrawableTextureIndices(model);
    const auto drawOrders = csmGetDrawableDrawOrders(model);
    const auto renderOrders = csmGetDrawableRenderOrders(model);
    const auto opacity = csmGetDrawableOpacities(model);
    const auto multiply = csmGetDrawableMultiplyColors(model);
    const auto screen = csmGetDrawableScreenColors(model);
    const auto parent = csmGetDrawableParentPartIndices(model);
    if (!ids || !vertices || !positions || !uvs || !indices || !indexCounts || !masks || !maskCounts || !flags ||
        !dynamic || !texture || !drawOrders || !renderOrders || !opacity || !multiply || !screen || !parent)
        return fail("Missing drawable arrays");
    for (int mesh = 0; mesh < drawables; ++mesh) {
        if (!ids[mesh] || vertices[mesh] < 0 || indexCounts[mesh] < 0 || maskCounts[mesh] < 0 ||
            (vertices[mesh] && (!positions[mesh] || !uvs[mesh])) || (indexCounts[mesh] && !indices[mesh]) ||
            (maskCounts[mesh] && !masks[mesh])) return fail("Invalid drawable buffers");
        DrawableState drawable;
        drawable.id = QString::fromUtf8(ids[mesh]);
        for (int vertex = 0; vertex < vertices[mesh]; ++vertex) {
            if (!std::isfinite(positions[mesh][vertex].X) || !std::isfinite(positions[mesh][vertex].Y) ||
                !std::isfinite(uvs[mesh][vertex].X) || !std::isfinite(uvs[mesh][vertex].Y)) return fail("Nonfinite drawable position or UV");
        }
        for (int index = 0; index < indexCounts[mesh]; ++index)
            if (indices[mesh][index] >= vertices[mesh]) return fail("Invalid drawable index");
        for (int mask = 0; mask < maskCounts[mesh]; ++mask) drawable.masks.append(masks[mesh][mask]);
        drawable.positions = Raw(positions[mesh], vertices[mesh]);
        drawable.uvs = Raw(uvs[mesh], vertices[mesh]);
        drawable.indices = Raw(indices[mesh], indexCounts[mesh]);
        if (!std::isfinite(opacity[mesh])) return fail("Nonfinite drawable opacity");
        for (const csmVector4 color : {multiply[mesh], screen[mesh]})
            if (!std::isfinite(color.X) || !std::isfinite(color.Y) || !std::isfinite(color.Z) || !std::isfinite(color.W))
                return fail("Nonfinite drawable color");
        drawable.scalar.append(char(flags[mesh]));
        drawable.scalar.append(char(dynamic[mesh] & csmIsVisible));
        for (int value : {texture[mesh], drawOrders[mesh], renderOrders[mesh], parent[mesh]}) drawable.scalar.append(Raw(&value, 1));
        drawable.scalar.append(Raw(opacity + mesh, 1));
        drawable.scalar.append(Raw(multiply + mesh, 1));
        drawable.scalar.append(Raw(screen + mesh, 1));
        state.drawables.append(std::move(drawable));
    }
    if (!owner.GuardsIntact()) return fail("Core wrote outside the supplied aligned buffers");
    result = std::move(state);
    error.clear();
    return true;
}

bool Same(const State& expected, const State& actual, QString& error)
{
    if (expected.parameterIds != actual.parameterIds || expected.parameterTypes != actual.parameterTypes ||
        expected.parameterKeys != actual.parameterKeys || expected.parameterMinimums != actual.parameterMinimums ||
        expected.parameterMaximums != actual.parameterMaximums || expected.parameterDefaults != actual.parameterDefaults ||
        expected.parameterValues != actual.parameterValues || expected.partIds != actual.partIds ||
        expected.partParents != actual.partParents || expected.partOpacities != actual.partOpacities || expected.canvas != actual.canvas ||
        expected.drawables.size() != actual.drawables.size()) {
        error = QStringLiteral("Parameter, part or canvas state differs");
        return false;
    }
    for (int index = 0; index < expected.drawables.size(); ++index) {
        const auto& left = expected.drawables[index];
        const auto& right = actual.drawables[index];
        for (const auto& field : {std::pair{"positions", left.positions == right.positions}, {"UVs", left.uvs == right.uvs},
                                 {"indices", left.indices == right.indices}, {"masks", left.masks == right.masks},
                                 {"scalar state", left.scalar == right.scalar}, {"ID", left.id == right.id}}) {
            if (!field.second) {
                error = QStringLiteral("Drawable %1 (%2): %3 differs").arg(index).arg(left.id, QString::fromLatin1(field.first));
                return false;
            }
        }
    }
    error.clear();
    return true;
}

QVector<QVector<float>> Poses(CoreModel& owner)
{
    const auto* model = owner.Get();
    const int count = csmGetParameterCount(model);
    const auto minimum = csmGetParameterMinimumValues(model), maximum = csmGetParameterMaximumValues(model);
    const auto defaults = csmGetParameterDefaultValues(model);
    QVector<float> base;
    for (int index = 0; index < count; ++index) base.append(defaults[index]);
    QVector<QVector<float>> result{base};
    for (double ratio : {0.0, 0.25, 0.5, 1.0}) {
        QVector<float> pose;
        for (int index = 0; index < count; ++index)
            pose.append(float(double(minimum[index]) * (1.0 - ratio) + double(maximum[index]) * ratio));
        result.append(pose);
    }
    for (int index = 0; index < count; ++index) {
        auto low = base, high = base;
        low[index] = minimum[index];
        high[index] = maximum[index];
        result.append(low);
        result.append(high);
    }
    return result;
}

std::pair<int, int> MatchingMeshes(const CModelDocument& document)
{
    const auto& meshes = document.Drawables();
    for (int first = 0; first < meshes.size(); ++first)
        for (int second = first + 1; second < meshes.size(); ++second)
            if (meshes[first].vertexCount > 0 && !meshes[first].keyforms.isEmpty() &&
                meshes[first].vertexCount == meshes[second].vertexCount && meshes[first].keyforms.size() == meshes[second].keyforms.size())
                return {first, second};
    return {-1, -1};
}
}

class RealCorePreviewTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qInfo("Real Cubism Core version: 0x%08x, latest MOC version: %u", csmGetVersion(), csmGetLatestMocVersion());
    }

    void originalModelsInitializeAndUpdate_data() { Models(); }
    void originalModelsInitializeAndUpdate()
    {
        QFETCH(QString, path);
        const auto source = Read(path);
        QString error;
        CoreModel core;
        QVERIFY2(core.Load(source, error), qPrintable(error));
        CModelDocument document;
        QVERIFY2(document.LoadBytes(source, path, &error), qPrintable(error));
        QCOMPARE(csmGetParameterCount(core.Get()), document.Parameters().size());
        QCOMPARE(csmGetDrawableCount(core.Get()), document.Drawables().size());
        const auto initial = csmGetParameterValues(core.Get());
        const auto defaults = csmGetParameterDefaultValues(core.Get());
        for (int index = 0; index < document.Parameters().size(); ++index) QCOMPARE(initial[index], defaults[index]);
        for (const auto& pose : Poses(core)) {
            core.Apply(pose);
            State snapshot;
            QVERIFY2(Capture(core, snapshot, error), qPrintable(error));
        }
    }

    void metadataPreservesEveryDrawableAtOriginalPoses_data() { Models(); }
    void metadataPreservesEveryDrawableAtOriginalPoses()
    {
        QFETCH(QString, path);
        const auto source = Read(path);
        QString error;
        CModelDocument document;
        QVERIFY2(document.LoadBytes(source, path, &error), qPrintable(error));
        QVERIFY(!document.Parameters().isEmpty());
        const auto& parameter = document.Parameters().front();
        auto metadata = document.ParameterMetadata(parameter.id);
        metadata.minimum -= 1.0f;
        metadata.maximum += 2.0f;
        metadata.defaultValue = float(double(metadata.minimum) * 0.25 + double(metadata.maximum) * 0.75);
        QVERIFY(document.SetParameterMetadata(parameter.id, metadata));
        QByteArray output;
        QVERIFY2(document.BuildPreviewMoc(output, &error), qPrintable(error));
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(source, &error));
        CMocPreviewBuilder::Overrides edits;
        edits.parameterMetadata.insert(parameter.id, metadata);
        QByteArray direct;
        QVERIFY2(builder.BuildOverrides(edits, direct, &error), qPrintable(error));
        QCOMPARE(output, direct);
        CoreModel baseline, candidate;
        QVERIFY2(baseline.Load(source, error), qPrintable(error));
        QVERIFY2(candidate.Load(output, error), qPrintable(error));
        QCOMPARE(csmGetParameterMinimumValues(candidate.Get())[0], metadata.minimum);
        QCOMPARE(csmGetParameterMaximumValues(candidate.Get())[0], metadata.maximum);
        QCOMPARE(csmGetParameterDefaultValues(candidate.Get())[0], metadata.defaultValue);
        QCOMPARE(csmGetParameterValues(candidate.Get())[0], metadata.defaultValue);
        for (const auto& pose : Poses(baseline)) {
            baseline.Apply(pose);
            candidate.Apply(pose);
            State expected, actual;
            QVERIFY2(Capture(baseline, expected, error), qPrintable(error));
            QVERIFY2(Capture(candidate, actual, error), qPrintable(error));
            expected.parameterMinimums[0] = metadata.minimum;
            expected.parameterMaximums[0] = metadata.maximum;
            expected.parameterDefaults[0] = metadata.defaultValue;
            QVERIFY2(Same(expected, actual, error), qPrintable(error));
        }
    }

    void maskGrowthShrinkAndClearPreserveEvaluation_data() { Models(); }
    void maskGrowthShrinkAndClearPreserveEvaluation()
    {
        QFETCH(QString, path);
        const auto source = Read(path);
        QString error;
        CModelDocument document;
        QVERIFY2(document.LoadBytes(source, path, &error), qPrintable(error));
        QVERIFY(document.Drawables().size() >= 4);
        const QString id = document.Drawables()[0].id;
        CoreModel baseline;
        QVERIFY2(baseline.Load(source, error), qPrintable(error));
        QByteArray previous = source;
        for (const QVector<int>& masks : {QVector<int>{1, 2, 3}, QVector<int>{2}, QVector<int>{}}) {
            QVERIFY(document.SetDrawableMaskIndices(id, masks));
            QByteArray output;
            QVERIFY2(document.BuildPreviewMoc(output, &error), qPrintable(error));
            CModelDocument chained;
            QVERIFY2(chained.LoadBytes(previous, path, &error), qPrintable(error));
            QVERIFY(chained.SetDrawableMaskIndices(id, masks));
            QByteArray rewritten;
            QVERIFY2(chained.BuildPreviewMoc(rewritten, &error), qPrintable(error));
            CoreModel candidate;
            QVERIFY2(candidate.Load(output, error), qPrintable(error));
            CoreModel sequential;
            QVERIFY2(sequential.Load(rewritten, error), qPrintable(error));
            QCOMPARE(csmGetDrawableMaskCounts(candidate.Get())[0], masks.size());
            for (const auto& pose : Poses(baseline)) {
                baseline.Apply(pose);
                candidate.Apply(pose);
                sequential.Apply(pose);
                State expected, actual, repeated;
                QVERIFY2(Capture(baseline, expected, error), qPrintable(error));
                QVERIFY2(Capture(candidate, actual, error), qPrintable(error));
                QVERIFY2(Capture(sequential, repeated, error), qPrintable(error));
                expected.drawables[0].masks = masks;
                QVERIFY2(Same(expected, actual, error), qPrintable(error));
                QVERIFY2(Same(expected, repeated, error), qPrintable(error));
            }
            previous = rewritten;
        }
        QVERIFY(document.SetDrawableMaskIndices(id, document.Drawables()[0].maskIndices));
        QByteArray restored;
        QVERIFY(document.BuildPreviewMoc(restored, &error));
        QCOMPARE(restored, source);
    }

    void sharedGeometryUsesIndependentCoreOracle_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("sharedKeyforms");
        for (const char* name : {"Mark", "Natori", "Rice", "Wanko"})
            for (bool keyforms : {false, true})
                QTest::newRow(qPrintable(QString::fromLatin1(name) + (keyforms ? "-keyforms" : "-positions")))
                    << QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3" << keyforms;
    }

    void sharedGeometryUsesIndependentCoreOracle()
    {
        QFETCH(QString, path);
        QFETCH(bool, sharedKeyforms);
        const auto original = Read(path);
        QString error;
        CModelDocument sourceDocument;
        QVERIFY2(sourceDocument.LoadBytes(original, path, &error), qPrintable(error));
        const auto [first, second] = MatchingMeshes(sourceDocument);
        QVERIFY2(first >= 0, "The real sample must contain equal-size independent mesh blocks");
        const auto& mesh = sourceDocument.Drawables()[first];
        const int firstRow = Cell(original, 0xcc, first), secondRow = Cell(original, 0xcc, second);
        QVERIFY(firstRow != secondRow);
        QByteArray shared = original, independent = original;
        const int rows = sharedKeyforms ? mesh.keyforms.size() : 1;
        for (int local = 0; local < rows; ++local) {
            const int a = Cell(original, 0x158, firstRow + local), b = Cell(original, 0x158, secondRow + local);
            QVERIFY(a != b);
            const int length = mesh.vertexCount * 2 * int(sizeof(float));
            independent.replace(Word(original, 0x15c) + b * 4, length,
                                original.mid(Word(original, 0x15c) + a * 4, length));
            if (sharedKeyforms) {
                for (int slot : {0x150, 0x154})
                    SetCell(independent, slot, secondRow + local, Cell(original, slot, firstRow + local));
            } else SetCell(shared, 0x158, secondRow, a);
        }
        if (sharedKeyforms) SetCell(shared, 0xcc, second, firstRow);
        CoreModel sharedBefore, independentBefore;
        QVERIFY2(sharedBefore.Load(shared, error), qPrintable(QStringLiteral("Aliased source: ") + error));
        QVERIFY2(independentBefore.Load(independent, error), qPrintable(QStringLiteral("Independent source: ") + error));
        const auto poses = Poses(sharedBefore);
        for (const auto& pose : poses) {
            sharedBefore.Apply(pose);
            independentBefore.Apply(pose);
            State expected, actual;
            QVERIFY2(Capture(sharedBefore, expected, error), qPrintable(error));
            QVERIFY2(Capture(independentBefore, actual, error), qPrintable(error));
            QVERIFY2(Same(expected, actual, error), qPrintable(error));
        }
        CModelDocument document;
        QVERIFY2(document.LoadBytes(shared, path, &error), qPrintable(error));
        auto changed = document.DrawableKeyformPositions(mesh.id, 0);
        for (int coordinate = 0; coordinate < changed.size(); ++coordinate)
            changed[coordinate] += coordinate % 2 ? -0.015625f : 0.03125f;
        QVERIFY(document.SetDrawableKeyformPositions(mesh.id, 0, changed));
        const int firstPosition = Cell(original, 0x158, firstRow);
        for (int coordinate = 0; coordinate < changed.size(); ++coordinate)
            PutFloat(independent, Word(original, 0x15c) + (firstPosition + coordinate) * 4, changed[coordinate]);
        QByteArray output;
        QVERIFY2(document.BuildPreviewMoc(output, &error), qPrintable(error));
        QVERIFY(output != shared);
        CoreModel candidate, oracle;
        QVERIFY2(oracle.Load(independent, error), qPrintable(error));
        QVERIFY2(candidate.Load(output, error), qPrintable(error));
        bool changedGeometry = false;
        for (const auto& pose : poses) {
            sharedBefore.Apply(pose);
            oracle.Apply(pose);
            candidate.Apply(pose);
            State before, expected, actual;
            QVERIFY2(Capture(sharedBefore, before, error), qPrintable(error));
            QVERIFY2(Capture(oracle, expected, error), qPrintable(error));
            QVERIFY2(Capture(candidate, actual, error), qPrintable(error));
            QVERIFY2(Same(expected, actual, error), qPrintable(error));
            changedGeometry = changedGeometry || before.drawables[first].positions != actual.drawables[first].positions;
            QCOMPARE(actual.drawables[second].positions, before.drawables[second].positions);
        }
        QVERIFY(changedGeometry);
        QByteArray repeated;
        QVERIFY(document.BuildPreviewMoc(repeated, &error));
        QCOMPARE(repeated, output);
        QCOMPARE(document.SourceBytes(), shared);
        QCOMPARE(document.SourceDigest(), QCryptographicHash::hash(shared, QCryptographicHash::Sha256));
    }

    void invalidRequestsPreservePreviousOutput_data() { Models(); }
    void invalidRequestsPreservePreviousOutput()
    {
        QFETCH(QString, path);
        const auto source = Read(path);
        QString error;
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(source, &error));
        CModelDocument document;
        QVERIFY(document.LoadBytes(source, path, &error));
        CMocPreviewBuilder::Overrides edits;
        const auto& mesh = document.Drawables().front();
        QVector<QVector<float>> forms;
        for (const auto& frame : mesh.keyforms) forms.append(frame.positions);
        QVERIFY(!forms.isEmpty() && !forms[0].isEmpty());
        forms[0][0] = std::numeric_limits<float>::quiet_NaN();
        edits.artMeshPositions.insert(mesh.id, forms);
        QByteArray output = source;
        QVERIFY(!builder.BuildOverrides(edits, output, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(output, source);
        edits = {};
        auto metadata = document.ParameterMetadata(document.Parameters().front().id);
        metadata.minimum = metadata.maximum + 1.0f;
        edits.parameterMetadata.insert(document.Parameters().front().id, metadata);
        QVERIFY(!builder.BuildOverrides(edits, output, &error));
        QCOMPARE(output, source);
        QVERIFY(builder.BuildOverrides({}, output, &error));
        QCOMPARE(output, source);
        CoreModel unchanged;
        QVERIFY2(unchanged.Load(output, error), qPrintable(error));
    }

    void core6OffscreenArraysHaveValidOwnershipAndMasks_data() { Models(); }
    void core6OffscreenArraysHaveValidOwnershipAndMasks()
    {
#if LIVE2D_CORE_API_VERSION < 6
        QSKIP("Core 6 API is not enabled");
#else
        QFETCH(QString, path);
        CoreModel core;
        QString error;
        const auto source = Read(path);
        QVERIFY2(core.Load(source, error), qPrintable(error));
        auto* model = core.Get();
        if (csmGetMocVersion(source.constData(), unsigned(source.size())) < csmMocVersion_53)
            QSKIP("The model does not contain Cubism 5.3 offscreen data");
        const int offscreens = csmGetOffscreenCount(model);
        QVERIFY(offscreens >= 0);
        if (!offscreens) QSKIP("The model has no offscreen surfaces");
        const int parts = csmGetPartCount(model);
        const int drawables = csmGetDrawableCount(model);
        const auto renderOrders = csmGetRenderOrders(model);
        const auto owners = csmGetOffscreenOwnerIndices(model);
        const auto blendModes = csmGetOffscreenBlendModes(model);
        const auto opacities = csmGetOffscreenOpacities(model);
        const auto multiply = csmGetOffscreenMultiplyColors(model);
        const auto screen = csmGetOffscreenScreenColors(model);
        const auto flags = csmGetOffscreenConstantFlags(model);
        const auto maskCounts = csmGetOffscreenMaskCounts(model);
        const auto masks = csmGetOffscreenMasks(model);
        const auto partOffscreens = csmGetPartOffscreenIndices(model);
        QVERIFY(renderOrders && owners && blendModes && opacities && multiply && screen && flags && maskCounts && masks);
        QVERIFY(partOffscreens || !parts);
        const int total = drawables + offscreens;
        QVector<bool> seen(total, false);
        for (int index = 0; index < total; ++index) {
            QVERIFY(renderOrders[index] >= 0 && renderOrders[index] < total);
            QVERIFY(!seen[renderOrders[index]]);
            seen[renderOrders[index]] = true;
        }
        for (int index = 0; index < offscreens; ++index) {
            QVERIFY(owners[index] >= 0 && owners[index] < parts);
            QVERIFY(std::isfinite(opacities[index]));
            for (const auto* color : {multiply + index, screen + index})
                for (float channel : {color->X, color->Y, color->Z, color->W}) QVERIFY(std::isfinite(channel));
            QVERIFY(maskCounts[index] >= 0);
            if (maskCounts[index]) QVERIFY(masks[index]);
            for (int mask = 0; mask < maskCounts[index]; ++mask)
                QVERIFY(masks[index][mask] >= -1 && masks[index][mask] < drawables);
        }
        for (int index = 0; index < parts; ++index)
            QVERIFY(partOffscreens[index] >= -1 && partOffscreens[index] < offscreens);
#endif
    }
};

QTEST_GUILESS_MAIN(RealCorePreviewTests)
#include "real_core_preview_tests.moc"
