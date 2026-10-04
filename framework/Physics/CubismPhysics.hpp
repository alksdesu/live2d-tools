#ifndef CUBISMPHYSICS_H
#define CUBISMPHYSICS_H


#include "framework/Math/CubismVector2.hpp"
#include "framework/Physics/CubismPhysicsInternal.hpp"
#include "framework/Physics/CubismPhysicsObservation.hpp"
#include <QVector>
#include <QString>
#include <QByteArray>
#include <QJsonObject>
#include <memory>

struct ParameterPhysicsReferences;

class PhysicsParameterDefinitionTests;

namespace Live2D { namespace Cubism { namespace Framework {

class CubismModel;
struct CubismPhysicsRig;

/**
 * @brief 物理演算クラス
 *
 * 物理演算のクラス。
 */
class CubismPhysics
{
    friend class ::PhysicsParameterDefinitionTests;
public:
    enum class StatePolicy { PreserveCompatible, Stabilize };
    class PreparedRuntimeState
    {
        friend class CubismPhysics;
    public:
        PreparedRuntimeState();
        ~PreparedRuntimeState();
        PreparedRuntimeState(PreparedRuntimeState&&) noexcept;
        PreparedRuntimeState& operator=(PreparedRuntimeState&&) noexcept;
        PreparedRuntimeState(const PreparedRuntimeState&) = delete;
        PreparedRuntimeState& operator=(const PreparedRuntimeState&) = delete;
        bool WasStabilized() const { return stabilized; }
    private:
        const CubismPhysics* owner = nullptr;
        quint64 revision = 0;
        bool stabilized = false;
        std::unique_ptr<CubismPhysics, void (*)(CubismPhysics*)> work;
    };
    class ParameterDefinitionState
    {
        friend class CubismPhysics;
        const CubismPhysics* owner = nullptr;
        QVector<float> parameters, inputs;
        QVector<int> inputIndices, outputIndices;
        QVector<quint8> activeInputs, activeOutputs;
        bool structure = false;
    };
    bool PrepareParameterDefinitionChange(const CubismModel& previous, const CubismModel& replacement,
                                         ParameterDefinitionState& result, QString* error = nullptr) const;
    void CommitParameterDefinitionChange(ParameterDefinitionState& state) noexcept;
    bool PrepareParameterStructureChange(const CubismModel& previous, const CubismModel& replacement,
                                         ParameterDefinitionState& result, QString* error = nullptr,
                                         const ParameterPhysicsReferences* references = nullptr) const;
    bool PrepareModelBinding(const CubismModel& model, const ParameterPhysicsReferences& references,
                             ParameterDefinitionState& result, QString* error = nullptr) const;
    bool PrepareRuntimeState(const CubismPhysics* previous, const CubismModel& previousModel,
                             const CubismModel& replacementModel, const ParameterPhysicsReferences& references,
                             PreparedRuntimeState& result, StatePolicy policy = StatePolicy::PreserveCompatible,
                             QString* error = nullptr) const;
    bool ValidatePreparedRuntimeState(const PreparedRuntimeState& state) const noexcept;
    bool CommitRuntimeState(PreparedRuntimeState& state) noexcept;
    const QByteArray& SourceDigest() const { return _sourceDigest; }
    bool SetObservationGroup(int groupIndex);
    bool CaptureObservation(const CubismModel& model, PhysicsObservation& result, QString* error = nullptr) const;
    bool CaptureOutputDiagnostics(QVector<PhysicsOutputDiagnostic>& result, QString* error = nullptr) const;
    bool RebindObservationModel(const CubismModel& previous, const CubismModel& replacement) noexcept;
    bool CaptureConfiguration(QJsonObject& result, QString* error = nullptr) const;
    bool HasFiniteRuntimeState(QString* error = nullptr) const;

    /**
     * @brief オプション
     *
     * 物理演算のオプション。
     */
    struct Options
    {
        CubismVector2 Gravity; ///< 重力方向
        CubismVector2 Wind; ///< 風の方向
    };

    /**
     * @brief 物理演算出力結果
     *
     * パラメータに適用する前の物理演算の出力結果
     */
    struct PhysicsOutput
    {
        csmVector<csmFloat32> outputs;
    };

    /**
     * @brief インスタンスの作成
     *
     * インスタンスを作成する。
     *
     * @param[in]   buffer      physics3.jsonが読み込まれいるバッファ
     * @param[in]   size        バッファのサイズ
     * @return  作成されたインスタンス
     */
    static CubismPhysics* Create(const csmByte* buffer, csmSizeInt size, QString* error = nullptr);

    /**
     * @brief インスタンスの破棄
     *
     * インスタンスを破棄する。
     *
     * @param[in]   physics     破棄するインスタンス
     */
    static void Delete(CubismPhysics* physics);

    /**
     * @brief パラメータのリセット
     *
     * パラメータをリセットする。
     */
    void Reset();

    /**
     * @brief 現在のパラメータ値で物理演算が安定化する状態を演算する。
     * @param[in]   model       物理演算の結果を適用するモデル
     */
    void Stabilization(CubismModel* model);

    /**
     * @brief 物理演算の評価
     *
     * 物理演算を評価する。
     *
     * @param[in]   model               物理演算の結果を適用するモデル
     * @param[in]   deltaTimeSeconds    デルタ時間[秒]
     */
    void Evaluate(CubismModel* model, csmFloat32 deltaTimeSeconds);

    /**
     * @brief オプションの設定
     *
     * オプションを設定する。
     *
     * @param[in]   options     オプション
     */
    void SetOptions(const Options& options);

    /**
     * @brief オプションの取得
     *
     * オプションを取得する。
     *
     * @return オプション
     */
    const Options& GetOptions() const;

private:
    /**
     * @brief コンストラクタ
     *
     * コンストラクタ。
     */
    CubismPhysics();

    /**
     * @brief デストラクタ
     *
     * デストラクタ。
     */
    virtual ~CubismPhysics();

    // Prevention of copy Constructor
    CubismPhysics(const CubismPhysics&);
    CubismPhysics& operator=(const CubismPhysics&);

    /**
     * @brief physics3.jsonのパース
     *
     * physics3.jsonをパースする。
     *
     * @param[in]   physicsJson     physics3.jsonが読み込まれているバッファ
     * @param[in]   size            バッファのサイズ
     */
    void Parse(const csmByte* physicsJson, csmSizeInt size);

    /**
     * @brief 初期化
     *
     * 初期化する。
     */
    void Initialize();

    /**
     * @brief 物理演算結果の適用
     *
     * 振り子演算の最新の結果と一つ前の結果から指定した重みで適用する。
     *
     * @param model 物理演算の結果を適用するモデル
     * @param weight 最新結果の重み
     */
    void Interpolate(CubismModel* model, csmFloat32 weight, PhysicsObservation* observation = nullptr);

    CubismPhysicsRig* _physicsRig; ///< 物理演算のデータ
    Options _options; ///< オプション

    csmVector<PhysicsOutput> _currentRigOutputs; ///< 最新の振り子計算の結果
    csmVector<PhysicsOutput> _previousRigOutputs; ///< 一つ前の振り子計算の結果

    csmFloat32 _currentRemainTime; ///< 物理演算が処理していない時間

    QVector<csmFloat32> _parameterCaches;      ///< Evaluateで利用するパラメータのキャッシュ
    QVector<csmFloat32> _parameterInputCaches; ///< UpdateParticlesが動くときの入力をキャッシュ
    QVector<quint8> _activeInputs, _activeOutputs;
    QByteArray _sourceDigest;
    QByteArray _normalizedBytes;
    QByteArray _topology;
    quint64 _stateRevision = 0;
    bool ResolveParameterIndices(CubismModel* model);
    bool StabilizeValues(const CubismModel& model, QVector<float>& values, QString* error = nullptr);
    void CopyDynamicStateFrom(const CubismPhysics& previous);
    int _observationGroup = -1;
    QString _observationGroupId;
    PhysicsObservation _observation;
    const CubismModel* _observationModel = nullptr;
    quint64 _observationSequence = 0;
    double _observationTime = 0;
    void InvalidateObservation() noexcept;
    bool PrepareObservation(const CubismModel& model, PhysicsObservation& result, QString* error = nullptr) const;
    bool BeginObservation(const CubismModel& model, csmFloat32 deltaTime, PhysicsObservation& result) noexcept;
    void FinishObservation(const CubismModel& model, PhysicsObservation& result) noexcept;

    csmBool _isJsonValid; ///< 正しくJsonデータが取得出来たか
};

}}}

#endif // CUBISMPHYSICS_H
