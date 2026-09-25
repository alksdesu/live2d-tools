#include "live2dstruct/keyform/clinearinterpolation.h"
#include "live2dstruct/tree/cpartsnode.h"

#include <QtTest>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

using namespace live2dstruct;

namespace {
class Parameters
{
public:
    Parameters() : reader(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"), table(reader) {}
    bool IsValid() const { return table.IsValid() && !table.parameters.empty() && !table.parameter_bindings.empty(); }

    CParameterNode* Add(const std::vector<float>& keys, float value)
    {
        const size_t slot = nodes.size() % table.parameter_bindings.size();
        CParameters definition = *table.parameters.front();
        definition.ids = QStringLiteral("Axis%1").arg(static_cast<qulonglong>(nodes.size()));
        definition.parameter_types = 0;
        definition.parameter_binding_sources_begin_indices = static_cast<int>(slot);
        definition.parameter_binding_sources_counts = 1;
        auto* binding = table.parameter_bindings[slot];
        binding->keys_sources_begin_indices = static_cast<int>(table.keys->values.size());
        binding->keys_sources_counts = static_cast<int>(keys.size());
        table.keys->values.insert(table.keys->values.end(), keys.begin(), keys.end());
        auto node = std::make_unique<CParameterNode>(definition.ids);
        node->InitParameterValue(table, definition, 0);
        node->SetCurValue(value);
        CParameterNode* result = node.get();
        nodes.push_back(std::move(node));
        return result;
    }

private:
    CReader reader;
    CSectionTable table;
    std::vector<std::unique_ptr<CParameterNode>> nodes;
};

CWarpNodeKeyform WarpKey(float opacity, std::vector<float> positions)
{
    return CWarpNodeKeyform(opacity, positions);
}

bool Near(float value, float expected)
{
    return std::isfinite(value) && std::abs(value - expected) < 0.00002f;
}
}

class KeyformInterpolationTests : public QObject
{
    Q_OBJECT
private slots:
    void unboundNodesCopyEveryField()
    {
        CRotationNode rotation("rotation");
        rotation.GetKeyFormS() = {CRotationNodeKeyform(0.25f, 720, 3, -4, -2, true, false)};
        QVERIFY(CLinearInterpolation::SetCKeyFormObject(&rotation));
        auto& key = rotation.GetCurKeyForm();
        QCOMPARE(key.Getopacities(), 0.25f);
        QCOMPARE(key.Getangles(), 720.0f);
        QCOMPARE(key.Getorigin_x(), 3.0f);
        QCOMPARE(key.Getorigin_y(), -4.0f);
        QCOMPARE(key.Getscales(), -2.0f);
        QVERIFY(key.Getis_reflect_x());
        QVERIFY(!key.Getis_reflect_y());
        CWarpNode warp("warp");
        warp.GetKeyFormS() = {WarpKey(0.4f, {1, 2, 3, 4})};
        QVERIFY(CLinearInterpolation::SetCKeyFormObject(&warp));
        QCOMPARE(warp.GetCurKeyForm().GetOpacities(), 0.4f);
        QVERIFY((warp.GetCurKeyForm().GetPosition() == std::vector<float>{1, 2, 3, 4}));
        CArtMeshNode mesh("mesh");
        mesh.GetKeyFormS() = {CArtMeshKeyForm(0.3f, 17.5f, {1, 2})};
        QVERIFY(CLinearInterpolation::SetCKeyFormObject(&mesh));
        QCOMPARE(mesh.GetCurKeyForm().getOpacities(), 0.3f);
        QCOMPARE(mesh.GetCurKeyForm().getDraw_orders(), 17.5f);
        rotation.GetKeyFormS().push_back(rotation.GetKeyFormS().front());
        QVERIFY(!CLinearInterpolation::SetCKeyFormObject(&rotation));
        QCOMPARE(key.Getangles(), 720.0f);
    }

    void singleAxisClampsAndKeepsUnwrappedAngles()
    {
        Parameters parameters;
        QVERIFY(parameters.IsValid());
        auto* axis = parameters.Add({0, 1}, 0.5f);
        CRotationNode rotation("rotation");
        rotation.AddFather(axis);
        rotation.GetKeyFormS() = {CRotationNodeKeyform(0.2f, 350, 0, 0, 2, true, false),
                                  CRotationNodeKeyform(0.8f, 10, 10, 20, 4, true, false)};
        QVERIFY(CLinearInterpolation::SetCKeyFormObject(&rotation));
        auto& key = rotation.GetCurKeyForm();
        QCOMPARE(key.Getangles(), 180.0f);
        QCOMPARE(key.Getscales(), 3.0f);
        QCOMPARE(key.Getorigin_x(), 5.0f);
        QCOMPARE(key.Getorigin_y(), 10.0f);
        QVERIFY(Near(key.Getopacities(), 0.5f));
        QVERIFY(key.Getis_reflect_x());
        QVERIFY(!key.Getis_reflect_y());
        for (float value : {-10.0f, 0.0f}) {
            axis->SetCurValue(value);
            QVERIFY(CLinearInterpolation::SetCKeyFormObject(&rotation));
            QCOMPARE(key.Getangles(), 350.0f);
            QCOMPARE(key.Getopacities(), 0.2f);
        }
        for (float value : {1.0f, 10.0f}) {
            axis->SetCurValue(value);
            QVERIFY(CLinearInterpolation::SetCKeyFormObject(&rotation));
            QCOMPARE(key.Getangles(), 10.0f);
            QCOMPARE(key.Getopacities(), 0.8f);
        }
    }

    void twoAxesPreserveFirstAxisFastestOrder()
    {
        Parameters parameters;
        QVERIFY(parameters.IsValid());
        CArtMeshNode mesh("mesh");
        CPartsNode part("part");
        mesh.AddFather(parameters.Add({0, 2}, 0.5f));
        mesh.AddFather(&part);
        mesh.AddFather(parameters.Add({-1, 1, 3}, 2));
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 2; ++x) {
                const float position = 10 * x + 100 * y;
                mesh.GetKeyFormS().emplace_back(0.1f + 0.1f * x + 0.2f * y, 5 * (x + 2 * y),
                                               std::vector<float>{position, -position});
            }
        QVERIFY(CLinearInterpolation::SetCKeyFormObject(&mesh));
        auto& key = mesh.GetCurKeyForm();
        QCOMPARE(key.GetPosition()[0], 152.5f);
        QCOMPARE(key.GetPosition()[1], -152.5f);
        QCOMPARE(key.getDraw_orders(), 16.25f);
        QVERIFY(Near(key.getOpacities(), 0.425f));
    }

    void threeAxesBlendAllEightCorners()
    {
        Parameters parameters;
        QVERIFY(parameters.IsValid());
        CArtMeshNode mesh("mesh");
        for (float value : {0.25f, 0.5f, 0.75f}) mesh.AddFather(parameters.Add({0, 1}, value));
        for (int z = 0; z < 2; ++z)
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x) {
                    const float value = x + 10 * y + 100 * z + 1000 * x * y * z;
                    mesh.GetKeyFormS().emplace_back(float(x + y + z) / 3, value * 10,
                                                   std::vector<float>{value, -value});
                }
        QVERIFY(CLinearInterpolation::SetCKeyFormObject(&mesh));
        QCOMPARE(mesh.GetCurKeyForm().GetPosition()[0], 174.0f);
        QCOMPARE(mesh.GetCurKeyForm().GetPosition()[1], -174.0f);
        QCOMPARE(mesh.GetCurKeyForm().getDraw_orders(), 1740.0f);
        QVERIFY(Near(mesh.GetCurKeyForm().getOpacities(), 0.5f));
    }

    void singletonAxesDoNotChangeFlattenedStride()
    {
        Parameters parameters;
        QVERIFY(parameters.IsValid());
        CWarpNode warp("warp");
        warp.AddFather(parameters.Add({0, 1}, 0.5f));
        warp.AddFather(parameters.Add({10}, -999));
        warp.AddFather(parameters.Add({0, 1}, 0.25f));
        warp.AddFather(parameters.Add({0, 1}, 0.75f));
        for (int index = 0; index < 8; ++index)
            warp.GetKeyFormS().push_back(WarpKey(index * 0.1f, {float(index), float(index + 1)}));
        QVERIFY(CLinearInterpolation::SetCKeyFormObject(&warp));
        QCOMPARE(warp.GetCurKeyForm().GetPosition()[0], 4.0f);
        QCOMPARE(warp.GetCurKeyForm().GetPosition()[1], 5.0f);
        QVERIFY(Near(warp.GetCurKeyForm().GetOpacities(), 0.4f));
        CRotationNode constant("constant");
        for (int index = 0; index < 30; ++index) constant.AddFather(parameters.Add({float(index)}, 42));
        constant.GetKeyFormS() = {CRotationNodeKeyform(0.6f, 37, 1, 2, 3, false, true)};
        QVERIFY(CLinearInterpolation::SetCKeyFormObject(&constant));
        QCOMPARE(constant.GetCurKeyForm().Getangles(), 37.0f);
        QVERIFY(constant.GetCurKeyForm().Getis_reflect_y());
    }

    void mixedReflectionsRejectOnlyContributingCorners()
    {
        Parameters parameters;
        QVERIFY(parameters.IsValid());
        auto* axis = parameters.Add({0, 1}, 0);
        CRotationNode rotation("rotation");
        rotation.AddFather(axis);
        rotation.GetKeyFormS() = {CRotationNodeKeyform(0.2f, 10, 1, 2, 1, false, false),
                                  CRotationNodeKeyform(0.8f, 20, 3, 4, 2, true, false)};
        QVERIFY(CLinearInterpolation::SetCKeyFormObject(&rotation));
        QVERIFY(!rotation.GetCurKeyForm().Getis_reflect_x());
        axis->SetCurValue(1);
        QVERIFY(CLinearInterpolation::SetCKeyFormObject(&rotation));
        QVERIFY(rotation.GetCurKeyForm().Getis_reflect_x());
        axis->SetCurValue(0.5f);
        QVERIFY(!CLinearInterpolation::SetCKeyFormObject(&rotation));
        QCOMPARE(rotation.GetCurKeyForm().Getangles(), 20.0f);
        QCOMPARE(rotation.GetCurKeyForm().Getopacities(), 0.8f);
        QVERIFY(rotation.GetCurKeyForm().Getis_reflect_x());
        auto* second = parameters.Add({0, 1}, 0);
        rotation.AddFather(second);
        rotation.GetKeyFormS() = {CRotationNodeKeyform(1, 10, 0, 0, 1, true, true),
                                  CRotationNodeKeyform(1, 20, 2, 4, 1, true, true),
                                  CRotationNodeKeyform(1, 30, 4, 8, 1, false, false),
                                  CRotationNodeKeyform(1, 40, 6, 12, 1, false, false)};
        QVERIFY(CLinearInterpolation::SetCKeyFormObject(&rotation));
        QCOMPARE(rotation.GetCurKeyForm().Getangles(), 15.0f);
        QVERIFY(rotation.GetCurKeyForm().Getis_reflect_x());
        QVERIFY(rotation.GetCurKeyForm().Getis_reflect_y());
        second->SetCurValue(0.5f);
        QVERIFY(!CLinearInterpolation::SetCKeyFormObject(&rotation));
        QCOMPARE(rotation.GetCurKeyForm().Getangles(), 15.0f);
    }

    void invalidAxesAndGridSizesPreserveCurrentKeyform()
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();
        const std::vector<std::vector<float>> invalid{{}, {1, 0}, {0, 0}, {0, nan}, {0, infinity}};
        Parameters parameters;
        QVERIFY(parameters.IsValid());
        for (const auto& keys : invalid) {
            CArtMeshNode mesh("mesh");
            CArtMeshKeyForm previous(0.4f, 17, {7, 9});
            mesh.SetCurMeshKey(previous);
            mesh.AddFather(parameters.Add(keys, 0.5f));
            mesh.GetKeyFormS() = {CArtMeshKeyForm(1, 1, {0, 0}), CArtMeshKeyForm(1, 2, {1, 1})};
            QVERIFY(!CLinearInterpolation::SetCKeyFormObject(&mesh));
            QCOMPARE(mesh.GetCurKeyForm().getDraw_orders(), 17.0f);
            QCOMPARE(mesh.GetCurKeyForm().getOpacities(), 0.4f);
            QVERIFY((mesh.GetCurKeyForm().GetPosition() == std::vector<float>{7, 9}));
        }
        CRotationNode rotation("rotation");
        auto* axis = parameters.Add({0, 1}, nan);
        rotation.AddFather(axis);
        rotation.GetKeyFormS() = {CRotationNodeKeyform(1, 10, 0, 0, 1, false, false),
                                  CRotationNodeKeyform(1, 20, 1, 1, 1, false, false)};
        QVERIFY(!CLinearInterpolation::SetCKeyFormObject(&rotation));
        axis->SetCurValue(0.5f);
        rotation.GetKeyFormS().pop_back();
        QVERIFY(!CLinearInterpolation::SetCKeyFormObject(&rotation));
        rotation.GetKeyFormS().clear();
        QVERIFY(!CLinearInterpolation::SetCKeyFormObject(&rotation));
        QVERIFY(!CLinearInterpolation::SetCKeyFormObject(nullptr));
        CPartsNode part("part");
        QVERIFY(!CLinearInterpolation::SetCKeyFormObject(&part));
    }

    void invalidKeyformFieldsFailAtomically()
    {
        Parameters parameters;
        QVERIFY(parameters.IsValid());
        auto* axis = parameters.Add({0, 1}, 0.5f);
        const float invalid = std::numeric_limits<float>::infinity();
        for (int field = 0; field < 5; ++field) {
            CRotationNode rotation("rotation");
            rotation.AddFather(axis);
            CRotationNodeKeyform previous(0.3f, 77, 7, 9, -2, true, true);
            rotation.SetCurKeyForm(previous);
            rotation.GetKeyFormS() = {CRotationNodeKeyform(1, 10, 0, 0, 1, false, false),
                                      CRotationNodeKeyform(1, 20, 1, 1, 1, false, false)};
            auto& broken = rotation.GetKeyFormS()[1];
            if (field == 0) broken.Setopacities(invalid);
            if (field == 1) broken.Setangles(invalid);
            if (field == 2) broken.Setscales(invalid);
            if (field == 3) broken.Setorigin_y(invalid);
            if (field == 4) broken.GetPosition().push_back(2);
            QVERIFY(!CLinearInterpolation::SetCKeyFormObject(&rotation));
            auto& key = rotation.GetCurKeyForm();
            QCOMPARE(key.Getopacities(), 0.3f);
            QCOMPARE(key.Getangles(), 77.0f);
            QCOMPARE(key.Getscales(), -2.0f);
            QVERIFY((key.GetPosition() == std::vector<float>{7, 9}));
            QVERIFY(key.Getis_reflect_x() && key.Getis_reflect_y());
        }
        CArtMeshNode mesh("mesh");
        mesh.AddFather(axis);
        CArtMeshKeyForm previous(0.3f, 77, {7, 9});
        mesh.SetCurMeshKey(previous);
        mesh.GetKeyFormS() = {CArtMeshKeyForm(1, 10, {0, 0}), CArtMeshKeyForm(1, invalid, {1, 1})};
        QVERIFY(!CLinearInterpolation::SetCKeyFormObject(&mesh));
        QCOMPARE(mesh.GetCurKeyForm().getDraw_orders(), 77.0f);
        CWarpNode warp("warp");
        warp.AddFather(axis);
        auto oldWarp = WarpKey(0.3f, {7, 9});
        warp.SetCurKeyForm(oldWarp);
        warp.GetKeyFormS() = {WarpKey(1, {0, 0}), WarpKey(invalid, {1, 1})};
        QVERIFY(!CLinearInterpolation::SetCKeyFormObject(&warp));
        QCOMPARE(warp.GetCurKeyForm().GetOpacities(), 0.3f);
        QVERIFY((warp.GetCurKeyForm().GetPosition() == std::vector<float>{7, 9}));
    }

    void finiteExtremeKeysAndFieldsDoNotOverflow()
    {
        Parameters parameters;
        QVERIFY(parameters.IsValid());
        const float maximum = std::numeric_limits<float>::max();
        CArtMeshNode mesh("mesh");
        mesh.AddFather(parameters.Add({-maximum, maximum}, 0));
        mesh.GetKeyFormS() = {CArtMeshKeyForm(maximum, -maximum, {-maximum, maximum}),
                              CArtMeshKeyForm(maximum, maximum, {maximum, -maximum})};
        QVERIFY(CLinearInterpolation::SetCKeyFormObject(&mesh));
        QCOMPARE(mesh.GetCurKeyForm().getOpacities(), maximum);
        QCOMPARE(mesh.GetCurKeyForm().getDraw_orders(), 0.0f);
        QVERIFY((mesh.GetCurKeyForm().GetPosition() == std::vector<float>{0, 0}));
    }
};

QTEST_GUILESS_MAIN(KeyformInterpolationTests)
#include "keyform_interpolation_tests.moc"
