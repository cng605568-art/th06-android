package io.github.cng605568art.th07;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;

/**
 * Turns decoded audio (16-bit little-endian PCM, any rate, 1+ channels) into the format the
 * game's music player reads: a WAV file of 16-bit PCM, 44,100 Hz, stereo. Near-silent frames
 * at both ends are trimmed so the song loops without a gap. Plain Java (no Android classes),
 * so it can be checked on a desktop JVM.
 */
final class PcmConverter {
    static final int RATE = 44100;
    static final int CHANNELS = 2;
    /** Samples at or below this magnitude count as silence when trimming the ends. */
    static final int SILENCE = 8;

    private PcmConverter() {}

    /** Progress callback: frames of source audio consumed so far. */
    interface Progress {
        void frames(long sourceFrames);
    }

    /**
     * Converts {@code raw} into {@code wav} and returns the number of output frames. Throws if
     * the audio is entirely silent.
     */
    static long convert(InputStream raw, int sourceRate, int sourceChannels, File wav, Progress progress)
            throws IOException {
        if (sourceRate < 1000 || sourceRate > 384000) throw new IOException("Unsupported sample rate " + sourceRate);
        if (sourceChannels < 1 || sourceChannels > 8) throw new IOException("Unsupported channel count " + sourceChannels);
        InputStream in = new BufferedInputStream(raw, 1 << 16);
        long frames;
        try (OutputStream out = new BufferedOutputStream(new FileOutputStream(wav), 1 << 16)) {
            out.write(header(0));
            Resampler resampler = new Resampler(sourceRate, out);
            byte[] frame = new byte[2 * sourceChannels];
            boolean started = false;
            long consumed = 0;
            while (readFully(in, frame)) {
                int l = sample(frame, 0);
                int r = sourceChannels >= 2 ? sample(frame, 1) : l;
                if (++consumed % 65536 == 0 && progress != null) progress.frames(consumed);
                if (!started) {  // leading silence (and decoder padding) is dropped
                    if (Math.abs(l) <= SILENCE && Math.abs(r) <= SILENCE) continue;
                    started = true;
                }
                resampler.push(l, r);
            }
            resampler.finish();
            frames = resampler.written();
        }
        if (frames == 0) {
            wav.delete();
            throw new IOException("The audio is silent");
        }
        // Trailing silence: find the last audible frame, cut the file there, write the header.
        try (RandomAccessFile f = new RandomAccessFile(wav, "rw")) {
            long last = lastAudibleFrame(f, frames);
            frames = last + 1;
            f.setLength(44 + frames * 4);
            f.seek(0);
            f.write(header(frames));
        }
        return frames;
    }

    /** The loop file next to a song: loop start and loop end in frames (little-endian int32). */
    static void writeLoop(File pos, long loopStart, long loopEnd) throws IOException {
        byte[] b = new byte[8];
        putInt(b, 0, (int) loopStart);
        putInt(b, 4, (int) loopEnd);
        try (OutputStream out = new FileOutputStream(pos)) {
            out.write(b);
        }
    }

    static byte[] header(long frames) {
        long data = frames * 4;
        byte[] h = new byte[44];
        ascii(h, 0, "RIFF");
        putInt(h, 4, (int) (36 + data));
        ascii(h, 8, "WAVE");
        ascii(h, 12, "fmt ");
        putInt(h, 16, 16);
        putShort(h, 20, 1);  // PCM
        putShort(h, 22, CHANNELS);
        putInt(h, 24, RATE);
        putInt(h, 28, RATE * CHANNELS * 2);
        putShort(h, 32, CHANNELS * 2);
        putShort(h, 34, 16);
        ascii(h, 36, "data");
        putInt(h, 40, (int) data);
        return h;
    }

    private static long lastAudibleFrame(RandomAccessFile f, long frames) throws IOException {
        final int chunk = 4096;
        byte[] buf = new byte[chunk * 4];
        long end = frames;
        while (end > 0) {
            long start = Math.max(0, end - chunk);
            int n = (int) (end - start);
            f.seek(44 + start * 4);
            f.readFully(buf, 0, n * 4);
            for (int i = n - 1; i >= 0; --i) {
                if (Math.abs(sample(buf, 2 * i)) > SILENCE || Math.abs(sample(buf, 2 * i + 1)) > SILENCE) return start + i;
            }
            end = start;
        }
        return 0;
    }

    private static boolean readFully(InputStream in, byte[] b) throws IOException {
        int at = 0;
        while (at < b.length) {
            int n = in.read(b, at, b.length - at);
            if (n < 0) {
                if (at == 0) return false;
                throw new EOFException("Truncated audio frame");
            }
            at += n;
        }
        return true;
    }

    private static int sample(byte[] b, int index) {
        return (short) ((b[2 * index] & 0xff) | (b[2 * index + 1] << 8));
    }

    private static void ascii(byte[] b, int at, String s) {
        for (int i = 0; i < 4; ++i) b[at + i] = (byte) s.charAt(i);
    }

    private static void putShort(byte[] b, int at, int v) {
        b[at] = (byte) v;
        b[at + 1] = (byte) (v >> 8);
    }

    private static void putInt(byte[] b, int at, int v) {
        putShort(b, at, v);
        putShort(b, at + 2, v >> 16);
    }

    /**
     * Streaming Catmull-Rom resampler to 44,100 Hz. At 44,100 Hz in, every output frame is the
     * input frame unchanged.
     */
    static final class Resampler {
        private final double step;
        private final OutputStream out;
        private final int[] ringL = new int[4];
        private final int[] ringR = new int[4];
        private final byte[] buf = new byte[4096 * 4];
        private int bufAt = 0;
        private long newest = -1;  // index of the newest input frame
        private long realEnd = Long.MAX_VALUE;  // last real input frame once finished
        private double pos = 0;  // next output position, in input frames
        private long written = 0;

        Resampler(int sourceRate, OutputStream out) {
            this.step = (double) sourceRate / RATE;
            this.out = out;
        }

        void push(int l, int r) throws IOException {
            ++newest;
            if (newest == 0) {  // the frame before the first one repeats it
                for (int k = 0; k < 4; ++k) {
                    ringL[k] = l;
                    ringR[k] = r;
                }
            }
            ringL[(int) (newest & 3)] = l;
            ringR[(int) (newest & 3)] = r;
            // Output positions between input frames newest-2 and newest-1 can be computed now.
            while (pos <= realEnd && (long) Math.floor(pos) + 2 <= newest) emit();
        }

        void finish() throws IOException {
            if (newest < 0) {
                flush();
                return;
            }
            realEnd = newest;
            int l = ringL[(int) (newest & 3)], r = ringR[(int) (newest & 3)];
            push(l, r);  // the last frame repeated, to interpolate up to it
            push(l, r);
            flush();
        }

        long written() {
            return written;
        }

        private void emit() throws IOException {
            long i = (long) Math.floor(pos);
            double t = pos - i;
            writeFrame(interpolate(ringL, i, t), interpolate(ringR, i, t));
            pos = ++written * step;
        }

        private static int interpolate(int[] ring, long i, double t) {
            double p0 = ring[(int) ((i - 1) & 3)], p1 = ring[(int) (i & 3)];
            double p2 = ring[(int) ((i + 1) & 3)], p3 = ring[(int) ((i + 2) & 3)];
            if (t == 0) return (int) p1;
            double v = p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
            return (int) Math.max(-32768, Math.min(32767, Math.round(v)));
        }

        private void writeFrame(int l, int r) throws IOException {
            if (bufAt == buf.length) flush();
            buf[bufAt++] = (byte) l;
            buf[bufAt++] = (byte) (l >> 8);
            buf[bufAt++] = (byte) r;
            buf[bufAt++] = (byte) (r >> 8);
        }

        private void flush() throws IOException {
            out.write(buf, 0, bufAt);
            bufAt = 0;
        }
    }
}
