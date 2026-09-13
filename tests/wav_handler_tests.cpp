#include "live2d/LAppWavFileHandler.hpp"
#include <QtTest>

class WavHandlerTests : public QObject
{
    Q_OBJECT
private slots:
    void normalizes32BitPcm()
    {
        Csm::csmByte sample[] = {0x00, 0x00, 0x00, 0xC0};
        const float value = LAppWavFileHandler::NormalizePcmSample(32, sample, 4);
        QVERIFY(qAbs(value + 0.5f) < 0.0001f);
    }
};

QTEST_GUILESS_MAIN(WavHandlerTests)
#include "wav_handler_tests.moc"
