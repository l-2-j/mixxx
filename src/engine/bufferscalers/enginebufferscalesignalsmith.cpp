#include "engine/bufferscalers/enginebufferscalesignalsmith.h"

#include "engine/readaheadmanager.h"
#include "moc_enginebufferscalesignalsmith.cpp"
#include "util/defs.h"
#include "util/sample.h"
#include "util/timer.h"

EngineBufferScaleSignalSmith::EngineBufferScaleSignalSmith(ReadAheadManager* pReadAheadManager)
        : m_pReadAheadManager(pReadAheadManager),
          m_buffers(),
          m_bufferPtrs(),
          m_interleavedBuffer(MAX_BUFFER_LEN) {
    onSignalChanged();
}

void EngineBufferScaleSignalSmith::setScaleParameters(
        double base_rate, double* pTempoRatio, double* pPitchRatio) {
    m_dBaseRate = base_rate;
    m_dTempoRatio = *pTempoRatio;
    m_dPitchRatio = *pPitchRatio;

    m_stretch.setTransposeFactor(static_cast<float>(m_dPitchRatio));
    m_stretch.setFormantFactor(1.0);
}

void EngineBufferScaleSignalSmith::onSignalChanged() {
    if (!getOutputSignal().isValid()) {
        return;
    }

    uint8_t channelCount = getOutputSignal().getChannelCount();
    if (m_buffers.size() != channelCount) {
        m_buffers.resize(channelCount);
    }

    if (m_bufferPtrs.size() != channelCount) {
        m_bufferPtrs.resize(channelCount);
    }

    for (int chIdx = 0; chIdx < channelCount; chIdx++) {
        if (m_buffers[chIdx].size() == MAX_BUFFER_LEN) {
            continue;
        }
        m_buffers[chIdx] = mixxx::SampleBuffer(MAX_BUFFER_LEN);
        m_bufferPtrs[chIdx] = m_buffers[chIdx].data();
    }

    // Configure stretcher with default settings
#if defined(Q_OS_ANDROID)
    // TODO expose both preset explicitly via settings?
    m_stretch.presetCheaper(channelCount, getOutputSignal().getSampleRate());
#else
    m_stretch.presetDefault(channelCount, getOutputSignal().getSampleRate());
#endif
    clear();
}

void EngineBufferScaleSignalSmith::clear() {
    m_stretch.reset();
    seek();
}

SINT EngineBufferScaleSignalSmith::fetchAndDeinterleave(SINT sampleToRead) {
    auto available_frames = getOutputSignal().samples2frames(
            m_pReadAheadManager->getNextSamples(
                    // The value doesn't matter here. All that matters is we
                    // are going forward or backward.
                    m_dBaseRate * m_dTempoRatio,
                    m_interleavedBuffer.data(),
                    sampleToRead,
                    getOutputSignal().getChannelCount()));

    switch (getOutputSignal().getChannelCount()) {
    case mixxx::audio::ChannelCount::stereo():
        SampleUtil::deinterleaveBuffer(
                m_buffers[0].data(),
                m_buffers[1].data(),
                m_interleavedBuffer.data(),
                available_frames);
        break;
    case mixxx::audio::ChannelCount::stem():
        SampleUtil::deinterleaveBuffer(
                m_buffers[0].data(),
                m_buffers[1].data(),
                m_buffers[2].data(),
                m_buffers[3].data(),
                m_buffers[4].data(),
                m_buffers[5].data(),
                m_buffers[6].data(),
                m_buffers[7].data(),
                m_interleavedBuffer.data(),
                available_frames);
        break;
    default: {
        int chCount = getOutputSignal().getChannelCount();
        // The sampler are ordered as following in pBuffer
        //    1234..X1234...X...
        // And need to be reordered as following
        // m_buffers#1 = 11..
        // m_buffers#2 = 22..
        // m_buffers#3 = 33..
        // m_buffers#4 = 44..fff
        // m_buffers#X = XX..
        //
        // Because of the unanticipated number of buffer and channel, we cannot
        // use any SampleUtil in this case
        for (SINT frameIdx = 0; frameIdx < available_frames; ++frameIdx) {
            for (int channel = 0; channel < chCount; channel++) {
                m_buffers[channel].data()[frameIdx] =
                        m_interleavedBuffer.data()[frameIdx * chCount + channel];
            }
        }
    } break;
    }
    return available_frames;
}

void EngineBufferScaleSignalSmith::seek() {
    if (!getOutputSignal().isValid() || (m_dPitchRatio == 0 && m_dTempoRatio == 0)) {
        return;
    }
    qDebug() << "Seek" << m_dPitchRatio << m_dTempoRatio;

    const SINT processing_latency_samples = getOutputSignal().frames2samples(
            static_cast<SINT>(m_stretch.inputLatency() + m_stretch.outputLatency()));
    const SINT available_frames = fetchAndDeinterleave(processing_latency_samples);

    {
        ScopedTimer t(QStringLiteral("Signalsmith::process"));
        m_stretch.seek(m_bufferPtrs.data(), available_frames, m_dTempoRatio);
    }
}

double EngineBufferScaleSignalSmith::scaleBuffer(CSAMPLE* pOutputBuffer, SINT iOutputBufferSize) {
    ScopedTimer t(QStringLiteral("EngineBufferScaleSignalsmith::scaleBuffer"));
    if (m_dBaseRate == 0.0 || m_dTempoRatio == 0.0) {
        SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
        // No actual samples/frames have been read from the
        // unscaled input buffer!
        return 0.0;
    }

    const SINT next_block_frames_required =
            // static_cast<SINT>(m_stretch.inputLatency()) +
            static_cast<SINT>(std::round(std::fabs(m_dTempoRatio) *
                    static_cast<double>(getOutputSignal().samples2frames(
                            iOutputBufferSize))));
    const SINT available_frames = fetchAndDeinterleave(getOutputSignal().frames2samples(
            next_block_frames_required));

    auto output_frame = getOutputSignal().samples2frames(iOutputBufferSize);
    float* outputBufferPtr[8] = {
            m_interleavedBuffer.data(),
            m_interleavedBuffer.data(iOutputBufferSize),
            m_interleavedBuffer.data(2 * iOutputBufferSize),
            m_interleavedBuffer.data(3 * iOutputBufferSize),
            m_interleavedBuffer.data(4 * iOutputBufferSize),
            m_interleavedBuffer.data(5 * iOutputBufferSize),
            m_interleavedBuffer.data(6 * iOutputBufferSize),
            m_interleavedBuffer.data(7 * iOutputBufferSize),
    };
    {
        ScopedTimer t(QStringLiteral("Signalsmith::process"));
        m_stretch.process(m_bufferPtrs.data(), available_frames, outputBufferPtr, output_frame);
    }

    switch (getOutputSignal().getChannelCount()) {
    case mixxx::audio::ChannelCount::stereo():
        SampleUtil::interleaveBuffer(pOutputBuffer,
                m_interleavedBuffer.data(),
                m_interleavedBuffer.data(iOutputBufferSize),
                output_frame);
        break;
    case mixxx::audio::ChannelCount::stem():
        SampleUtil::interleaveBuffer(pOutputBuffer,
                m_interleavedBuffer.data(),
                m_interleavedBuffer.data(iOutputBufferSize),
                m_interleavedBuffer.data(2 * iOutputBufferSize),
                m_interleavedBuffer.data(3 * iOutputBufferSize),
                m_interleavedBuffer.data(4 * iOutputBufferSize),
                m_interleavedBuffer.data(5 * iOutputBufferSize),
                m_interleavedBuffer.data(6 * iOutputBufferSize),
                m_interleavedBuffer.data(7 * iOutputBufferSize),
                output_frame);
        break;
    default: {
        int chCount = getOutputSignal().getChannelCount();
        // The buffers samples are ordered as following
        //  m_buffers#1 = 11..
        //  m_buffers#2 = 22..
        //  m_buffers#3 = 33..
        //  m_buffers#4 = 44..
        //  m_buffers#X = XX..
        // And need to be reordered as following in pBuffer
        //  1234..X1234...X...
        //
        // Because of the unanticipated number of buffer and channel, we cannot
        // use any SampleUtil in this case
        for (SINT frameIdx = 0;
                frameIdx < getOutputSignal().samples2frames(iOutputBufferSize);
                ++frameIdx) {
            for (int channel = 0; channel < chCount; channel++) {
                pOutputBuffer[frameIdx * chCount + channel] =
                        m_buffers[channel].data()[frameIdx];
            }
        }
    } break;
    }

    // readFramesProcessed is interpreted as the total number of frames
    // consumed to produce the scaled buffer. Due to this, we do not take into
    // account directionality or starting point.
    return available_frames;
}
