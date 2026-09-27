#include "live2dstruct/cmotiontimeline.h"
#include "live2dstruct/cmotionkeyclipboard.h"
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
#include <limits>
#include <memory>

namespace {
using Timeline = CMotionTimeline;
using Mode = Timeline::TangentMode;
using Side = Timeline::HandleSide;
const CMotionTrack track{CMotionTarget::Parameter, "A"};
bool load(Timeline& timeline, bool restricted = false, const QJsonArray& segments = {0, 0, 1, .2, .4, .7, .6, 1, 1, 1, 1.6, 1.9, 2.5, 2, 3, 2}) {
    return timeline.RestoreState({{"version", 1}, {"playhead", 0}, {"motion", QJsonObject{{"Version", 3}, {"Extra", "preserved"},
        {"Meta", QJsonObject{{"Duration", 3}, {"Fps", 30}, {"AreBeziersRestricted", restricted}, {"CustomMeta", true}}},
        {"Curves", QJsonArray{QJsonObject{{"Target", "Parameter"}, {"Id", "A"}, {"Segments", segments}, {"CustomCurve", 11}}}}}}});
}
bool mode(Timeline& timeline, int index, Mode mode, QString* error = nullptr) { Timeline::BatchEdit edit; edit.tangents.append({{track,index},mode,{}}); Timeline::BatchResult result; return timeline.EditPoints(edit,result,error); }
QJsonObject state(const Timeline& timeline) { QJsonObject result; timeline.SaveState(&result, false, 0); return result; }
QPointF handle(const Timeline& timeline, int index, Side side) { return timeline.TangentHandle({track,index},side).value_or(QPointF()); }
bool close(double a, double b) { return std::abs(a-b) <= 1e-11 * std::max({1.0,std::abs(a),std::abs(b)}); }
}
class MotionTangentTests : public QObject {
    Q_OBJECT
private slots:
    void oldCurvesStayBrokenAndKeepNumbers() {
        Timeline timeline; QVERIFY(load(timeline,true)); const auto original=state(timeline); QVERIFY(!timeline.HasAuthoringTangents()); QCOMPARE(original["version"].toInt(),1);
        for(int i=0;i<3;++i) QCOMPARE(timeline.Tangent(track,i).mode,Mode::Broken);
        Timeline copy; QVERIFY(copy.RestoreState(original)); QCOMPARE(state(copy),original);
        QVERIFY(mode(timeline,1,Mode::Broken)); QCOMPARE(state(timeline),original);
    }
    void alignedUsesSlopeWithoutMirroringLengths() {
        Timeline timeline; QVERIFY(load(timeline)); QString error; QVERIFY2(mode(timeline,1,Mode::Aligned,&error),qPrintable(error));
        const auto tangent=timeline.Tangent(track,1); QVERIFY(close(tangent.slope,4.0/3)); QVERIFY(close(tangent.incomingFraction,.3)); QVERIFY(close(tangent.outgoingFraction,.3));
        QVERIFY(close(handle(timeline,1,Side::Outgoing).y(),1.8));
        Timeline::BatchEdit edit; edit.handles.append({{track,1},Side::Outgoing,{1.8,2.6},false}); Timeline::BatchResult result;
        QVERIFY2(timeline.EditPoints(edit,result,&error),qPrintable(error)); QVERIFY(close(timeline.Tangent(track,1).slope,2)); QVERIFY(close(handle(timeline,1,Side::Incoming).y(),.4));
        QVERIFY(close(timeline.Tangent(track,1).outgoingFraction,.4));
    }
    void temporaryLinkDoesNotChangeBrokenModeAndNoopAutoStaysAuto() {
        Timeline timeline; QVERIFY(load(timeline)); Timeline::BatchEdit edit; edit.handles.append({{track,1},Side::Outgoing,{1.6,2.2},true}); Timeline::BatchResult result; QString error;
        QVERIFY2(timeline.EditPoints(edit,result,&error),qPrintable(error)); QCOMPARE(timeline.Tangent(track,1).mode,Mode::Broken); QVERIFY(close(handle(timeline,1,Side::Incoming).y(),.4));
        QVERIFY(mode(timeline,1,Mode::AutoClamped)); const auto before=state(timeline); edit.handles[0].position=handle(timeline,1,Side::Outgoing);
        QVERIFY(timeline.EditPoints(edit,result)); QVERIFY(!result.changed); QCOMPARE(state(timeline),before);
        edit.handles[0].position.ry()+=.2; QVERIFY(timeline.EditPoints(edit,result)); QCOMPARE(timeline.Tangent(track,1).mode,Mode::Aligned);
    }
    void flatRemainsFlatAfterMovingAndOnlyHorizontalHandleChanges() {
        Timeline timeline; QVERIFY(load(timeline)); QVERIFY(mode(timeline,1,Mode::Flat)); Timeline::BatchEdit edit; edit.updates.append({{track,1},{1.5,4}}); Timeline::BatchResult result;
        QVERIFY(timeline.EditPoints(edit,result)); QCOMPARE(handle(timeline,1,Side::Incoming).y(),4.0); QCOMPARE(handle(timeline,1,Side::Outgoing).y(),4.0);
        const auto before=state(timeline); edit={}; auto target=handle(timeline,1,Side::Outgoing); target.ry()+=1; edit.handles.append({{track,1},Side::Outgoing,target,false});
        QVERIFY(timeline.EditPoints(edit,result)); QVERIFY(!result.changed); QCOMPARE(state(timeline),before);
        edit.handles[0].position.rx()+=.1; QVERIFY(timeline.EditPoints(edit,result)); QCOMPARE(handle(timeline,1,Side::Outgoing).y(),4.0);
    }
    void automaticNonuniformSlopeAndExtremaAreIndependentKnownValues() {
        Timeline timeline; QVERIFY(load(timeline)); QVERIFY(mode(timeline,1,Mode::AutoClamped));
        QVERIFY(close(handle(timeline,1,Side::Incoming).x(),2.0/3)); QVERIFY(close(handle(timeline,1,Side::Incoming).y(),10.0/13));
        QVERIFY(close(handle(timeline,1,Side::Outgoing).x(),5.0/3)); QVERIFY(close(handle(timeline,1,Side::Outgoing).y(),19.0/13));
        Timeline::BatchEdit edit; edit.updates.append({{track,2},{3,0}}); Timeline::BatchResult result; QVERIFY(timeline.EditPoints(edit,result));
        QCOMPARE(handle(timeline,1,Side::Incoming).y(),1.0); QCOMPARE(handle(timeline,1,Side::Outgoing).y(),1.0);
        QVERIFY(mode(timeline,0,Mode::AutoClamped)); QVERIFY(mode(timeline,2,Mode::AutoClamped));
        for(int i=0;i<=300;++i) { const auto value=timeline.Evaluate(track,i/100.f); QVERIFY(value>=-1e-5 && value<=1.00001); }
    }
    void insertionDeletionReorderAndDormantModesPreserveIdentity() {
        Timeline timeline; QVERIFY(load(timeline)); QVERIFY(mode(timeline,1,Mode::Aligned)); const auto tangent=timeline.Tangent(track,1);
        Timeline::BatchEdit edit; edit.insertions.append({track,{.5,.5}}); Timeline::BatchResult result; QVERIFY(timeline.EditPoints(edit,result)); QCOMPARE(result.oldToNew[track][1],2); QCOMPARE(timeline.Tangent(track,2),tangent);
        edit={}; edit.removes={{track,1}}; QVERIFY(timeline.EditPoints(edit,result)); QCOMPARE(timeline.Tangent(track,1),tangent);
        QVERIFY(timeline.SetSegmentType(track,1,0)); QVERIFY(timeline.SetSegmentType(track,2,0)); QVERIFY(!timeline.Capabilities({track,1}).incoming.available); QCOMPARE(timeline.Tangent(track,1),tangent);
        QVERIFY(timeline.SetSegmentType(track,1,1)); QVERIFY(timeline.SetSegmentType(track,2,1)); QVERIFY(close((1-handle(timeline,1,Side::Incoming).y())/(1-handle(timeline,1,Side::Incoming).x()),tangent.slope));
        edit={}; edit.removes={{track,0},{track,2}}; QVERIFY(timeline.EditPoints(edit,result)); QCOMPARE(timeline.Tangent(track,0),tangent); QVERIFY(timeline.HasAuthoringTangents());
    }
    void affineTransformScalesSlopeAndActualCurveTogether_data() { QTest::addColumn<bool>("authored"); QTest::newRow("broken")<<false;QTest::newRow("aligned")<<true; }
    void affineTransformScalesSlopeAndActualCurveTogether() {
        QFETCH(bool,authored); Timeline timeline; QVERIFY(load(timeline)); if(authored) QVERIFY(mode(timeline,1,Mode::Aligned)); const auto before=timeline;
        Timeline::PointTransform transform;transform.selection={{track,0},{track,1},{track,2}};transform.timeScale=2;transform.valueScale=-3;transform.offset={1,4}; Timeline::BatchResult result;QString error;
        QVERIFY2(timeline.TransformPoints(transform,result,&error),qPrintable(error));
        if(authored) QVERIFY(close(timeline.Tangent(track,1).slope,before.Tangent(track,1).slope*-1.5));
        for(int i=0;i<=60;++i) { const float t=i/20.f; QVERIFY(std::abs(timeline.Evaluate(track,t*2+1)-(-3*before.Evaluate(track,t)+4))<3e-5f); }
    }
    void restrictedModesKeepRawXAndConvertWithoutChangingEvaluation() {
        Timeline timeline; QVERIFY(load(timeline,true)); const auto raw=state(timeline)["motion"].toObject()["Curves"].toArray()[0].toObject()["Segments"].toArray(); QVERIFY(mode(timeline,1,Mode::Aligned));
        auto segments=state(timeline)["motion"].toObject()["Curves"].toArray()[0].toObject()["Segments"].toArray(); QCOMPARE(segments[3],raw[3]);QCOMPARE(segments[5],raw[5]); QVERIFY(timeline.AreBeziersRestricted());
        Timeline::BatchEdit edit; auto target=handle(timeline,1,Side::Incoming);target.rx()-=.1;edit.handles.append({{track,1},Side::Incoming,target,false});Timeline::BatchResult result;const auto original=timeline;QString error;
        QVERIFY(!timeline.EditPoints(edit,result,&error)); QVERIFY(timeline.HasSameContent(original));
        QVERIFY(timeline.ConvertToUnrestricted(&error)); for(int i=0;i<=30;++i) QVERIFY(std::abs(timeline.Evaluate(track,i/10.f)-original.Evaluate(track,i/10.f))<1e-5f);
    }
    void stateV2AndStandardNumbersHaveSeparateContracts() {
        Timeline timeline; QVERIFY(load(timeline)); QVERIFY(mode(timeline,1,Mode::Flat)); const auto snapshot=state(timeline); QCOMPARE(snapshot["version"].toInt(),2); QVERIFY(snapshot.contains("tangents"));
        Timeline restored;QString error;QVERIFY2(restored.RestoreState(snapshot,nullptr,&error),qPrintable(error));QCOMPARE(state(restored),snapshot);
        QByteArray motion;QVERIFY(timeline.ToJson(motion,30,false,&error));const auto standard=QJsonDocument::fromJson(motion).object();QVERIFY(!standard.contains("tangents")); QCOMPARE(standard["Extra"],QJsonValue("preserved"));
        Timeline plain;QVERIFY(plain.RestoreState({{"version",1},{"motion",standard},{"playhead",0}}));QVERIFY(!plain.HasAuthoringTangents());
        for(int i=0;i<=60;++i) QCOMPARE(plain.Evaluate(track,i/20.f),timeline.Evaluate(track,i/20.f));
        auto bad=snapshot;auto modes=bad["tangents"].toArray();auto row=modes[0].toObject();auto values=row["keys"].toArray();auto key=values[0].toObject();auto mode=key["state"].toObject();mode["slope"]=12;key["state"]=mode;values[0]=key;row["keys"]=values;modes[0]=row;bad["tangents"]=modes;
        QVERIFY(!restored.RestoreState(bad,nullptr,&error));QCOMPARE(state(restored),snapshot);
    }
    void parameterArchiveAndBakingPreserveOrResetOnlyIntendedTracks() {
        Timeline timeline;QVERIFY(load(timeline));QVERIFY(mode(timeline,1,Mode::Aligned));const auto original=timeline;Timeline::ParameterTracks archived;QString error;
        QVERIFY(timeline.RemoveParameterTracks({"A"},archived,&error));QVERIFY(!timeline.HasAuthoringTangents());QVERIFY(timeline.RestoreParameterTracks(archived,&error));QVERIFY(timeline.HasSameContent(original));
        QVERIFY(timeline.ReplaceParameterSamples({{"B",{{0,1},{1,2}}}},&error));QCOMPARE(timeline.Tangent(track,1),original.Tangent(track,1));
        QVERIFY(timeline.ReplaceParameterSamples({{"A",{{0,0},{1,1}}}},&error));QVERIFY(!timeline.HasAuthoringTangents());
    }
    void clipboardV2KeepsIsolatedManualIntentAndV1RemainsSupported() {
        Timeline timeline;QVERIFY(load(timeline));QVERIFY(mode(timeline,1,Mode::Aligned));QByteArray bytes;QString error;
        QVERIFY(CMotionKeyClipboard::Capture(timeline,{{track,1}},bytes,&error));QCOMPARE(QJsonDocument::fromJson(bytes).object()["version"].toInt(),2);
        Timeline blank;CMotionKeyClipboard::Result result;QVERIFY2(CMotionKeyClipboard::Paste(blank,bytes,2,{},false,result,&error),qPrintable(error));QCOMPARE(result.timeline.Tangent(track,0),timeline.Tangent(track,1));
        QVERIFY(mode(timeline,1,Mode::Broken));QVERIFY(CMotionKeyClipboard::Capture(timeline,{{track,1}},bytes,&error));QCOMPARE(QJsonDocument::fromJson(bytes).object()["version"].toInt(),1);
        QVERIFY(CMotionKeyClipboard::Paste(blank,bytes,2,{},false,result,&error));QVERIFY(!result.timeline.HasAuthoringTangents());
    }
    void modeOnlyChangeIsAnAuthoringEditWithIdenticalStandardMotion() {
        Timeline timeline;QVERIFY(load(timeline,false,{0,0,1,.2,0,.7,0,1,0,1,1.6,0,2.5,0,3,0}));const auto before=timeline;QByteArray oldMotion,newMotion;
        QVERIFY(timeline.ToJson(oldMotion,30,false));Timeline::BatchEdit edit;edit.tangents.append({{track,1},Mode::Flat,{}});Timeline::BatchResult result;
        QVERIFY(timeline.EditPoints(edit,result));QVERIFY(result.changed);QVERIFY(!timeline.HasSameContent(before));QVERIFY(timeline.HasAuthoringTangents());
        QVERIFY(timeline.ToJson(newMotion,30,false));QCOMPARE(newMotion,oldMotion);QVERIFY(result.affected.contains({track,1}));
        QVERIFY(mode(timeline,1,Mode::Broken));QVERIFY(!timeline.HasAuthoringTangents());QCOMPARE(state(timeline)["version"].toInt(),1);
    }
    void automaticSamplesMatchRationalHermiteOracle() {
        Timeline timeline;QVERIFY(load(timeline));for(int i=0;i<3;++i)QVERIFY(mode(timeline,i,Mode::AutoClamped));
        QVERIFY(std::abs(timeline.Evaluate(track,.5f)-349.0/624)<2e-6);QVERIFY(std::abs(timeline.Evaluate(track,2.f)-509.0/312)<2e-6);
        QVERIFY(close(handle(timeline,0,Side::Outgoing).y(),7.0/18));QVERIFY(close(handle(timeline,2,Side::Incoming).y(),17.0/9));
    }
    void modeIdentitySeparatesTargetsAndParameterArchive() {
        Timeline timeline;QVERIFY(load(timeline));auto snapshot=state(timeline);auto motion=snapshot["motion"].toObject();auto curves=motion["Curves"].toArray();auto base=curves[0].toObject();
        base["Target"]="Model";curves.append(base);base["Target"]="PartOpacity";curves.append(base);motion["Curves"]=curves;snapshot["motion"]=motion;QVERIFY(timeline.RestoreState(snapshot));
        const CMotionTrack model{CMotionTarget::Model,"A"},part{CMotionTarget::PartOpacity,"A"};Timeline::BatchEdit edit;
        edit.tangents={{{track,1},Mode::Aligned,{}},{{model,1},Mode::Flat,{}},{{part,1},Mode::AutoClamped,{}}};Timeline::BatchResult result;QVERIFY(timeline.EditPoints(edit,result));
        Timeline::ParameterTracks archived;QVERIFY(timeline.RemoveParameterTracks({"A"},archived));QCOMPARE(timeline.Tangent(model,1).mode,Mode::Flat);QCOMPARE(timeline.Tangent(part,1).mode,Mode::AutoClamped);
        QVERIFY(timeline.RestoreParameterTracks(archived));QCOMPARE(timeline.Tangent(track,1).mode,Mode::Aligned);QCOMPARE(timeline.Tracks().size(),3);
    }
    void recordingAndMetadataChangesKeepExistingAuthoringStates() {
        qint64 clock=0;Timeline timeline([&]{return clock;});QVERIFY(load(timeline));QVERIFY(mode(timeline,1,Mode::Aligned));const auto tangent=timeline.Tangent(track,1);const auto before=timeline.BezierControls(track,1);
        QVERIFY(timeline.SetTiming(60,4));QVERIFY(timeline.SetFadeTimes({.1,.2}));QVERIFY(timeline.SetEvents({QJsonObject{{"Time",3.5},{"Value","event"}}}));QCOMPARE(timeline.Tangent(track,1),tangent);QCOMPARE(timeline.BezierControls(track,1),before);
        timeline.StartRecording();clock=100;QVERIFY(timeline.RecordValues({{"A",4},{"B",2}}));timeline.StopRecording();QCOMPARE(timeline.Tangent(track,1),tangent);
        QCOMPARE(timeline.Tangent(track,timeline.Tracks()[track].size()-1).mode,Mode::Broken);QCOMPARE(timeline.Tangent({CMotionTarget::Parameter,"B"},0).mode,Mode::Broken);
        const auto snapshot=state(timeline);Timeline restored;QVERIFY(restored.RestoreState(snapshot));QCOMPARE(state(restored),snapshot);QCOMPARE(restored.Tangent(track,1),tangent);
    }
    void sparseModesDoNotExpandDefaultTrackState() {
        Timeline timeline;QVector<QPointF> points;for(int i=0;i<20000;++i)points.append({double(i),0});QVERIFY(timeline.ReplaceParameterSamples({{"A",points}}));
        QVERIFY(timeline.SetSegmentType(track,10001,1));QVERIFY(mode(timeline,10000,Mode::Flat));const auto snapshot=state(timeline);
        const auto tangents=snapshot["tangents"].toArray();QCOMPARE(tangents.size(),1);QCOMPARE(tangents[0].toObject()["keys"].toArray().size(),1);
        QVERIFY(QJsonDocument(tangents).toJson(QJsonDocument::Compact).size()<512);Timeline restored;QVERIFY(restored.RestoreState(snapshot));QCOMPARE(restored.Tangent(track,10000).mode,Mode::Flat);
    }
    void shortUnrestrictedSpanFailsWithoutDamagingImportedMotion() {
        const double end=std::nextafter(1.f,std::numeric_limits<float>::infinity());const QJsonArray segments{1,0,1,1,0,end,1,end,1};
        Timeline timeline;QVERIFY(load(timeline,false,segments));const auto before=state(timeline);QString error;QVERIFY(!mode(timeline,0,Mode::AutoClamped,&error));QCOMPARE(state(timeline),before);
        Timeline restricted;QVERIFY(load(restricted,true,segments));QVERIFY2(mode(restricted,0,Mode::AutoClamped,&error),qPrintable(error));
        QVERIFY(!restricted.ConvertToUnrestricted(&error));QVERIFY(restricted.AreBeziersRestricted());
    }
    void malformedAuthoringStateIsRejectedAtomically_data() {
        QTest::addColumn<QString>("fault");for(auto name:{"unknown","track","duplicate-track","duplicate-key","negative-key","large-key","mode","ratio","slope","inconsistent","version"})QTest::newRow(name)<<QString(name);
    }
    void malformedAuthoringStateIsRejectedAtomically() {
        QFETCH(QString,fault);Timeline timeline;QVERIFY(load(timeline));QVERIFY(mode(timeline,1,Mode::Aligned));const auto original=state(timeline);auto bad=original;auto tracks=bad["tangents"].toArray();auto row=tracks[0].toObject();auto keys=row["keys"].toArray();auto key=keys[0].toObject();auto tangent=key["state"].toObject();
        if(fault=="unknown")tangent["extra"]=1;if(fault=="track")row["id"]="missing";if(fault=="negative-key")key["index"]=-1;if(fault=="large-key")key["index"]=99;
        if(fault=="mode")tangent["mode"]=9;if(fault=="ratio")tangent["incomingFraction"]=2;if(fault=="slope")tangent["slope"]=QJsonValue::Null;if(fault=="inconsistent")tangent["slope"]=42;
        key["state"]=tangent;keys[0]=key;if(fault=="duplicate-key")keys.append(key);row["keys"]=keys;tracks[0]=row;if(fault=="duplicate-track")tracks.append(row);bad["tangents"]=tracks;if(fault=="version")bad["version"]=1;
        QString error;float time=7;QVERIFY(!timeline.RestoreState(bad,&time,&error));QVERIFY(!error.isEmpty());QCOMPARE(time,7.f);QCOMPARE(state(timeline),original);
    }
    void restrictedClipboardAndArchivedTracksKeepTheirEffectiveSlopes() {
        Timeline source;QVERIFY(load(source,true));QVERIFY(mode(source,1,Mode::Aligned));QByteArray bytes;QVERIFY(CMotionKeyClipboard::Capture(source,{{track,0},{track,1},{track,2}},bytes));
        Timeline blank;CMotionKeyClipboard::Result pasted;QString error;QVERIFY2(CMotionKeyClipboard::Paste(blank,bytes,0,{},false,pasted,&error),qPrintable(error));
        QCOMPARE(pasted.timeline.Tangent(track,1).mode,Mode::Aligned);for(int i=0;i<=30;++i)QVERIFY(std::abs(source.Evaluate(track,i/10.f)-pasted.timeline.Evaluate(track,i/10.f))<1e-5f);
        Timeline::ParameterTracks archived;QVERIFY(source.RemoveParameterTracks({"A"},archived));QVERIFY(source.ConvertToUnrestricted());QVERIFY(source.RestoreParameterTracks(archived,&error));
        for(int i=0;i<=30;++i)QVERIFY(std::abs(source.Evaluate(track,i/10.f)-pasted.timeline.Evaluate(track,i/10.f))<1e-5f);
    }
    void flatHandleCannotCollapseInAuthorOrRuntimeTime() {
        Timeline timeline;QVERIFY(load(timeline,false,{0,0,1,0,0,.5,1,1,1}));QVERIFY(mode(timeline,0,Mode::Flat));const auto before=state(timeline);
        Timeline::BatchEdit edit;edit.handles.append({{track,0},Side::Outgoing,{0,0},false});Timeline::BatchResult result;QString error;
        QVERIFY(!timeline.EditPoints(edit,result,&error));QCOMPARE(state(timeline),before);QVERIFY(error.contains("时间长度"));
        edit.handles[0].position.setX(1e-46);QVERIFY(!timeline.EditPoints(edit,result,&error));QCOMPARE(state(timeline),before);
        const double end=std::nextafter(1.f,std::numeric_limits<float>::infinity());const QJsonArray tiny{1,0,1,1,0,end,1,end,1};
        Timeline free;QVERIFY(load(free,false,tiny));const auto original=state(free);QVERIFY(!mode(free,0,Mode::Flat,&error));QCOMPARE(state(free),original);
        Timeline restricted;QVERIFY(load(restricted,true,tiny));QVERIFY(mode(restricted,0,Mode::Flat,&error));const auto validRestricted=state(restricted);
        QVERIFY(!restricted.ConvertToUnrestricted(&error));QCOMPARE(state(restricted),validRestricted);
    }
    void flatCollapsedPersistentAndClipboardInputsAreRejected() {
        Timeline timeline;QVERIFY(load(timeline));QVERIFY(mode(timeline,1,Mode::Flat));const auto original=state(timeline);auto bad=original;
        auto tracks=bad["tangents"].toArray();auto row=tracks[0].toObject();auto keys=row["keys"].toArray();auto key=keys[0].toObject();auto tangent=key["state"].toObject();tangent["outgoingFraction"]=0;key["state"]=tangent;keys[0]=key;row["keys"]=keys;tracks[0]=row;bad["tangents"]=tracks;
        QString error;QVERIFY(!timeline.RestoreState(bad,nullptr,&error));QCOMPARE(state(timeline),original);
        QByteArray bytes;QVERIFY(CMotionKeyClipboard::Capture(timeline,{{track,0},{track,1},{track,2}},bytes,&error));auto clip=QJsonDocument::fromJson(bytes).object();auto curves=clip["tracks"].toArray();auto curve=curves[0].toObject();auto points=curve["points"].toArray();auto point=points[1].toObject();auto mode=point["tangent"].toObject();mode["incomingFraction"]=0;point["tangent"]=mode;points[1]=point;curve["points"]=points;curves[0]=curve;clip["tracks"]=curves;
        CMotionKeyClipboard::Result result;result.timeline=timeline;Timeline blank;QVERIFY(!CMotionKeyClipboard::Paste(blank,QJsonDocument(clip).toJson(),0,{},false,result,&error));QCOMPARE(state(result.timeline),original);
        mode["incomingFraction"]=1e-46;point["tangent"]=mode;points[1]=point;curve["points"]=points;curves[0]=curve;clip["tracks"]=curves;
        QVERIFY(!CMotionKeyClipboard::Paste(blank,QJsonDocument(clip).toJson(),0,{},false,result,&error));QCOMPARE(state(result.timeline),original);
    }
    void savingAuthoringStateDoesNotCopyUserRecordingCallbacks() {
        struct Clock {
            std::shared_ptr<int> copies;
            explicit Clock(std::shared_ptr<int> value):copies(std::move(value)){}
            Clock(const Clock& other):copies(other.copies){++*copies;}
            qint64 operator()() const{return 0;}
        };
        auto copies=std::make_shared<int>(0);Timeline timeline{std::function<qint64()>(Clock{copies})};QVERIFY(load(timeline));QVERIFY(mode(timeline,1,Mode::AutoClamped));
        const int before=*copies;QJsonObject saved;QVERIFY(timeline.SaveState(&saved,false,0));QCOMPARE(*copies,before);QCOMPARE(saved["version"].toInt(),2);
    }
    void invalidMixedRequestsAndCollapsedTimesAreAtomic() {
        Timeline timeline;QVERIFY(load(timeline));QVERIFY(mode(timeline,1,Mode::Aligned));const auto before=state(timeline);Timeline::BatchEdit edit;
        edit.handles.append({{track,1},Side::Incoming,{1,.5},false});Timeline::BatchResult result;result.changed=true;QString error;QVERIFY(!timeline.EditPoints(edit,result,&error));QCOMPARE(state(timeline),before);QVERIFY(result.changed);
        edit={};edit.tangents={{{track,1},Mode::Flat,{}},{{track,1},Mode::Broken,{}}};QVERIFY(!timeline.EditPoints(edit,result,&error));QCOMPARE(state(timeline),before);
        Timeline::PointTransform transform;transform.selection={{track,0},{track,1},{track,2}};transform.timeScale=0;QVERIFY(!timeline.TransformPoints(transform,result,&error));QCOMPARE(state(timeline),before);
        transform.timeScale=std::numeric_limits<double>::quiet_NaN();QVERIFY(!timeline.TransformPoints(transform,result,&error));QCOMPARE(state(timeline),before);
    }
};
QTEST_GUILESS_MAIN(MotionTangentTests)
#include "motion_tangent_tests.moc"
