#include "live2dstruct/cmotionkeyclipboard.h"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <limits>
#include <cmath>

namespace {
using Codec = CMotionKeyClipboard;
using Selection = CMotionPointSelection;
CMotionTrack parameter(const QString& id) { return {CMotionTarget::Parameter, id}; }
bool restore(CMotionTimeline& timeline, bool restricted, const QJsonArray& curves)
{
    return timeline.RestoreState({{"version",1},{"playhead",0},{"motion",QJsonObject{
        {"Version",3},{"RootExtra","keep"},{"Curves",curves},
        {"Meta",QJsonObject{{"Fps",59.940000123},{"Duration",8},{"Loop",true},{"AreBeziersRestricted",restricted},{"FadeInTime",.7},{"MetaExtra",17}}},
        {"UserData",QJsonArray{QJsonObject{{"Time",7},{"Value","event"},{"Extra",true}}}}
    }}});
}
QJsonObject curve(const QString& id, const QJsonArray& segments, const QString& target = "Parameter")
{ return {{"Target",target},{"Id",id},{"Segments",segments},{"FadeOutTime",.9},{"CurveExtra",id}}; }
QByteArray sample()
{
    CMotionTimeline source; source.AddPoint("A",1,2); source.AddPoint("A",2,4);
    QByteArray bytes; Codec::Capture(source,{{parameter("A"),0},{parameter("A"),1}},bytes); return bytes;
}
QByteArray encode(const QJsonObject& root) { return QJsonDocument(root).toJson(QJsonDocument::Compact); }
}

class MotionKeyClipboardTests : public QObject
{
    Q_OBJECT
private slots:
    void typedTracksKeepGlobalOffsetsAndTargetMetadata()
    {
        CMotionTimeline source; const CMotionTrack a=parameter("Opacity"), b{CMotionTarget::Model,"Opacity"}, c{CMotionTarget::PartOpacity,"Opacity"};
        for(const auto& track:QVector<CMotionTrack>{a,b,c}) { QVERIFY(source.AddPoint(track,1+int(track.target),.25)); QVERIFY(source.AddPoint(track,3+int(track.target),.75)); }
        Selection selection{{a,1},{c,0},{b,1},{a,0},{c,1},{b,0}};
        QByteArray bytes, ordered; QString error;
        QVERIFY2(Codec::Capture(source,selection,bytes,&error),qPrintable(error)); std::sort(selection.begin(),selection.end()); QVERIFY(Codec::Capture(source,selection,ordered)); QCOMPARE(bytes,ordered);
        const auto root=QJsonDocument::fromJson(bytes).object(); QCOMPARE(root.keys().size(),4); QVERIFY(!root.contains("events")); QVERIFY(!root.contains("fades"));
        CMotionTimeline target; QVERIFY(restore(target,false,{curve("Keep",{0,7,0,2,9})})); const auto before=target;
        Codec::Result result; QVERIFY2(Codec::Paste(target,bytes,10,{},false,result,&error),qPrintable(error)); QVERIFY(result.changed); QVERIFY(!result.convertedToUnrestricted);
        QVERIFY(target.HasSameContent(before)); QCOMPARE(result.timeline.Fps(),before.Fps()); QCOMPARE(result.timeline.Events(),before.Events()); QVERIFY(result.timeline.Loop());
        QCOMPARE(result.timeline.Tracks().value(parameter("Keep")),before.Tracks().value(parameter("Keep")));
        QCOMPARE(result.selection.size(),6);
        for(const auto& track:QVector<CMotionTrack>{a,b,c}) {
            const auto points=result.timeline.Tracks().value(track); QCOMPARE(points.size(),2);
            QCOMPARE(points[0].x(),10.0+int(track.target)); QCOMPARE(points[1].x(),12.0+int(track.target));
            QVERIFY(!result.timeline.TrackFadeTimes(track).fadeOut); QVERIFY(!result.timeline.TrackFadeTimes(track).fadeIn);
        }
        QByteArray output; QVERIFY(result.timeline.ToJson(output,result.timeline.Fps(),true)); const auto document=QJsonDocument::fromJson(output).object();
        QCOMPARE(document.value("RootExtra").toString(),QString("keep")); QCOMPARE(document.value("Meta").toObject().value("MetaExtra").toInt(),17);
    }

    void contiguousBezierRunsAndSingleDestination_data()
    {
        QTest::addColumn<bool>("restricted"); QTest::newRow("free")<<false; QTest::newRow("restricted")<<true;
    }
    void contiguousBezierRunsAndSingleDestination()
    {
        QFETCH(bool,restricted); CMotionTimeline source;
        QVERIFY(restore(source,restricted,{curve("A",{1,0,1,1.2,.8,1.8,.2,2,1,1,2.2,1.5,2.8,.5,3,2})}));
        QByteArray bytes; QString error; QVERIFY(Codec::Capture(source,{{parameter("A"),1},{parameter("A"),2}},bytes,&error));
        CMotionTimeline target; Codec::Result result; const CMotionTrack destination{CMotionTarget::PartOpacity,"Other"};
        QVERIFY2(Codec::Paste(target,bytes,5,destination,false,result,&error),qPrintable(error));
        QCOMPARE(result.timeline.SegmentType(destination,0),0); QCOMPARE(result.timeline.SegmentType(destination,1),1);
        QVERIFY(!result.timeline.TrackFadeTimes(destination).fadeOut); QVERIFY(result.timeline.Events().isEmpty());
        for(int n=0;n<=20;++n) {
            const float relative=float(n)/20;
            QVERIFY(std::abs(result.timeline.Evaluate(destination,5+relative)-source.Evaluate("A",2+relative))<.00001f);
        }
        QVERIFY(Codec::Capture(source,{{parameter("A"),0},{parameter("A"),2}},bytes));
        QVERIFY(Codec::Paste(target,bytes,0,{},false,result,&error)); QCOMPARE(result.timeline.SegmentType(parameter("A"),1),0);
        QCOMPARE(result.timeline.BezierControls(parameter("A"),1).size(),0);
    }

    void unrestrictedPasteConvertsExistingTargetWithoutChangingItsCurve()
    {
        CMotionTimeline source,target; QVERIFY(restore(source,false,{curve("B",{0,0,1,.1,1,.9,0,1,1})}));
        QVERIFY(restore(target,true,{curve("A",{0,0,1,.05,.9,.2,.1,2,1})}));
        const auto before=target; QByteArray bytes; QVERIFY(Codec::Capture(source,{{parameter("B"),0},{parameter("B"),1}},bytes));
        Codec::Result result; QString error; QVERIFY2(Codec::Paste(target,bytes,3,{},false,result,&error),qPrintable(error));
        QVERIFY(result.convertedToUnrestricted); QVERIFY(result.changed); QVERIFY(!result.timeline.AreBeziersRestricted()); QVERIFY(target.AreBeziersRestricted());
        for(int n=0;n<=40;++n) { const float t=float(n)/20; QVERIFY(std::abs(result.timeline.Evaluate("A",t)-before.Evaluate("A",t))<.00001f); }
        QCOMPARE(result.timeline.BezierControls(parameter("A"),1)[0].x(),2.0/3); QCOMPARE(result.timeline.BezierControls(parameter("A"),1)[1].x(),4.0/3);
        QCOMPARE(result.timeline.Events(),target.Events()); QCOMPARE(result.timeline.TrackFadeTimes(parameter("A")).fadeOut,target.TrackFadeTimes(parameter("A")).fadeOut);
    }

    void explicitCoincidentReplacementAndNoop()
    {
        CMotionTimeline source; QVERIFY(source.AddPoint("A",1,2)); QVERIFY(source.AddPoint("A",2,4)); QByteArray bytes; QVERIFY(Codec::Capture(source,{{parameter("A"),0},{parameter("A"),1}},bytes));
        Codec::Result result; QString error; QVERIFY(!Codec::Paste(source,bytes,1,{},false,result,&error));
        QVERIFY2(Codec::Paste(source,bytes,1,{},true,result,&error),qPrintable(error)); QVERIFY(!result.changed); QCOMPARE(result.selection.size(),2); QVERIFY(result.timeline.HasSameContent(source));
        CMotionTimeline target; QVERIFY(target.AddPoint("A",1,10)); QVERIFY(target.AddPoint("A",1.5,30)); QVERIFY(target.AddPoint("A",2,20));
        QVERIFY(Codec::Paste(target,bytes,1,{},true,result)); QCOMPARE(result.timeline.Tracks().value(parameter("A")),QVector<QPointF>({{1,2},{1.5,30},{2,4}}));
        QCOMPARE(target.Tracks().value(parameter("A"))[0].y(),10.0);
    }

    void doubleOffsetsRemainPrecise()
    {
        CMotionTimeline source; CMotionTimeline::BatchEdit input;
        input.insertions={{parameter("A"),{10.1234567890123,.123456789012345},0,{},{}},{parameter("A"),{11.2345678901234,.987654321098765},0,{},{}}};
        CMotionTimeline::BatchResult inserted; QString error; QVERIFY2(source.EditPoints(input,inserted,&error),qPrintable(error));
        QByteArray bytes; QVERIFY(Codec::Capture(source,inserted.inserted,bytes)); Codec::Result result; CMotionTimeline target;
        const double time=.987654321012345; QVERIFY(Codec::Paste(target,bytes,time,{},false,result,&error));
        const auto points=result.timeline.Tracks().value(parameter("A")); QCOMPARE(points[0].x(),time);
        QCOMPARE(points[1].x(),time+(input.insertions[1].point.x()-input.insertions[0].point.x())); QCOMPARE(points[0].y(),input.insertions[0].point.y());
    }

    void failuresKeepBothTimelineAndResult()
    {
        CMotionTimeline source; QVERIFY(source.AddPoint("A",0,1)); QVERIFY(source.AddPoint("A",1,2)); const auto before=source;
        Codec::Result result; QVERIFY(result.timeline.AddPoint("Sentinel",0,9)); result.selection={{parameter("Sentinel"),0}}; result.changed=true; result.convertedToUnrestricted=true;
        const auto prior=result.timeline; const auto selection=result.selection; QString error;
        for(double time:{-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN(),double(std::numeric_limits<float>::max())*2}) {
            QVERIFY(!Codec::Paste(source,sample(),time,{},true,result,&error)); QVERIFY(!error.isEmpty());
            QVERIFY(source.HasSameContent(before)); QVERIFY(result.timeline.HasSameContent(prior)); QVERIFY(result.selection==selection); QVERIFY(result.changed&&result.convertedToUnrestricted);
        }
        QByteArray output("keep"); QVERIFY(!Codec::Capture(source,{},output,&error)); QCOMPARE(output,QByteArray("keep"));
        QVERIFY(!Codec::Capture(source,{{parameter("A"),0},{parameter("A"),0}},output)); QCOMPARE(output,QByteArray("keep"));
        QVERIFY(!Codec::Capture(source,{{parameter("A"),2}},output)); QCOMPARE(output,QByteArray("keep"));
        QVERIFY(!Codec::Paste(source,QByteArray(Codec::MaximumBytes+1,' '),0,{},false,result,&error)); QVERIFY(result.timeline.HasSameContent(prior));
        source.StartRecording(); QVERIFY(!Codec::Paste(source,sample(),5,{},false,result,&error)); QVERIFY(source.IsRecording()); QVERIFY(result.timeline.HasSameContent(prior));
    }

    void rejectsFloatCollisionsWithoutImplicitReplacement()
    {
        CMotionTimeline source; QVERIFY(source.AddPoint("A",0,1)); QVERIFY(source.AddPoint("A",1,2)); QByteArray bytes; QVERIFY(Codec::Capture(source,{{parameter("A"),0},{parameter("A"),1}},bytes));
        CMotionTimeline target; Codec::Result result; QString error;
        QVERIFY(!Codec::Paste(target,bytes,1e30,{},true,result,&error)); QVERIFY(!error.isEmpty());
        QVERIFY(target.AddPoint("A",1,9)); CMotionTimeline one; QVERIFY(one.AddPoint("A",0,2)); QVERIFY(Codec::Capture(one,{{parameter("A"),0}},bytes));
        QVERIFY(!Codec::Paste(target,bytes,1.00000000001,{},true,result,&error)); QCOMPARE(target.Tracks().value(parameter("A"))[0].y(),9.0);
    }

    void redirectsOnlySingleTracksAndRejectsConversionOnFailureAtomically()
    {
        CMotionTimeline source; QVERIFY(source.AddPoint("A",0,1)); QVERIFY(source.AddPoint("B",0,2)); QByteArray bytes;
        QVERIFY(Codec::Capture(source,{{parameter("A"),0},{parameter("B"),0}},bytes));
        CMotionTimeline target; QVERIFY(restore(target,true,{curve("A",{0,0,1,.1,.8,.2,.2,1,1})})); const auto before=target;
        Codec::Result output; QVERIFY(output.timeline.AddPoint("Sentinel",0,3)); const auto old=output.timeline; QString error;
        QVERIFY(!Codec::Paste(target,bytes,0,parameter("C"),false,output,&error)); QVERIFY(output.timeline.HasSameContent(old));
        QVERIFY(Codec::Capture(source,{{parameter("A"),0}},bytes));
        QVERIFY(!Codec::Paste(target,bytes,0,{},false,output,&error)); QVERIFY(target.HasSameContent(before)); QVERIFY(target.AreBeziersRestricted()); QVERIFY(output.timeline.HasSameContent(old));
    }

    void byteLimitIsCheckedBeforeParsing()
    {
        auto bytes=sample(); bytes.append(QByteArray(Codec::MaximumBytes-bytes.size(),' '));
        CMotionTimeline target; Codec::Result result; QString error; QVERIFY2(Codec::Paste(target,bytes,0,{},false,result,&error),qPrintable(error));
        const auto previous=result.timeline; bytes.append(' '); QVERIFY(!Codec::Paste(target,bytes,0,{},false,result,&error)); QVERIFY(result.timeline.HasSameContent(previous));
    }

    void malformedPayloads_data()
    {
        QTest::addColumn<QByteArray>("payload"); const auto valid=QJsonDocument::fromJson(sample()).object();
        QTest::newRow("invalid-json")<<QByteArray("{"); QTest::newRow("array-root")<<QByteArray("[]");
        const auto add=[&](const char* label,const std::function<void(QJsonObject&)>& change){auto root=valid;change(root);QTest::newRow(label)<<encode(root);};
        add("future-version",[](auto&r){r["version"]=2;}); add("wrong-format",[](auto&r){r["format"]="other";});
        add("unknown-field",[](auto&r){r["events"]=QJsonArray{};}); add("wrong-mode",[](auto&r){r["restricted"]=1;}); add("empty",[](auto&r){r["tracks"]=QJsonArray{};});
        add("duplicate-track",[](auto&r){auto a=r["tracks"].toArray();a.append(a[0]);r["tracks"]=a;});
        const auto trackChange=[&](const char* label,const std::function<void(QJsonObject&)>& change){add(label,[&](auto&r){auto a=r["tracks"].toArray();auto t=a[0].toObject();change(t);a[0]=t;r["tracks"]=a;});};
        trackChange("unknown-target",[](auto&t){t["target"]="Unknown";}); trackChange("null-id",[](auto&t){t["id"]=QString("A")+QChar::Null;}); trackChange("number-id",[](auto&t){t["id"]=7;});
        trackChange("extra-track-field",[](auto&t){t["fades"]=0;}); trackChange("empty-points",[](auto&t){t["points"]=QJsonArray{};});
        const auto pointChange=[&](const char* label,const std::function<void(QJsonObject&)>& change){trackChange(label,[&](auto&t){auto a=t["points"].toArray();auto p=a[1].toObject();change(p);a[1]=p;t["points"]=a;});};
        pointChange("duplicate-time",[](auto&p){p["time"]=0;}); pointChange("negative-time",[](auto&p){p["time"]=-1;}); pointChange("nonfinite-value",[](auto&p){p["value"]=QJsonValue();});
        pointChange("fractional-type",[](auto&p){p["segment"]=1.5;}); pointChange("unknown-type",[](auto&p){p["segment"]=99;}); pointChange("bad-start",[](auto&p){p["start"]=QJsonArray{0,999};});
        pointChange("missing-controls",[](auto&p){p["segment"]=1;}); pointChange("wrong-controls",[](auto&p){p["controls"]=QJsonArray{};});
        pointChange("controls-outside",[](auto&p){p["segment"]=1;p["controls"]=QJsonArray{QJsonArray{-.1,0},QJsonArray{.7,1}};});
        pointChange("controls-reversed",[](auto&p){p["segment"]=1;p["controls"]=QJsonArray{QJsonArray{.8,0},QJsonArray{.2,1}};});
        pointChange("extra-point-field",[](auto&p){p["events"]=QJsonArray{};});
        trackChange("first-segment-without-start",[](auto&t){auto points=t["points"].toArray();auto p=points[0].toObject();p["segment"]=3;points[0]=p;t["points"]=points;});
    }
    void malformedPayloads()
    {
        QFETCH(QByteArray,payload); CMotionTimeline source; QVERIFY(source.AddPoint("Keep",0,4)); const auto before=source;
        Codec::Result result; QVERIFY(result.timeline.AddPoint("Sentinel",0,8)); const auto previous=result.timeline; QString error;
        QVERIFY(!Codec::Paste(source,payload,0,{},false,result,&error)); QVERIFY(!error.isEmpty()); QVERIFY(source.HasSameContent(before)); QVERIFY(result.timeline.HasSameContent(previous));
    }

    void pointBudgetIncludesRetainedTargetTracks()
    {
        CMotionTimeline source; CMotionTimeline::BatchEdit edit; edit.insertions.reserve(CMotionTimeline::MaximumPointCount);
        for(int i=0;i<CMotionTimeline::MaximumPointCount;++i) edit.insertions.append({parameter("A"),{double(i),double(i%7)},0,{},{}});
        CMotionTimeline::BatchResult added; QString error; QVERIFY2(source.EditPoints(edit,added,&error),qPrintable(error));
        QByteArray bytes; QVERIFY2(Codec::Capture(source,added.inserted,bytes,&error),qPrintable(error)); QVERIFY(bytes.size()<=Codec::MaximumBytes);
        CMotionTimeline empty; Codec::Result result; QVERIFY2(Codec::Paste(empty,bytes,0,{},false,result,&error),qPrintable(error)); QCOMPARE(result.timeline.TotalPointCount(),CMotionTimeline::MaximumPointCount);
        const auto previous=result.timeline; QVERIFY(empty.AddPoint("Keep",0,3)); QVERIFY(!Codec::Paste(empty,bytes,0,{},false,result,&error)); QVERIFY(result.timeline.HasSameContent(previous));
    }
};
QTEST_GUILESS_MAIN(MotionKeyClipboardTests)
#include "motion_key_clipboard_tests.moc"
