package io.github.cng605568art.th06;

import android.content.Context;
import android.media.AudioFormat;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.net.Uri;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Locale;

/**
 * "Replace music": the player's own audio file (MP3, or anything else Android can decode)
 * becomes one of the game's songs. It is decoded with the platform's decoders, converted to
 * the game's music format (WAV, 16-bit PCM, 44,100 Hz, stereo) and kept in the app's storage
 * as music/th06_NN.wav, with a th06_NN.pos loop file: the engine plays it in place of the
 * game folder's track and loops it the way the game loops its music (the whole song, or from
 * a chosen loop point to the end). Nothing in the game folder is changed.
 */
final class MusicReplacer {
    /** The game's 17 songs (bgm/th06_01 .. th06_17), by where they play. */
    static final String[] TRACKS = {
        "01  Title screen", "02  Stage 1", "03  Stage 1 boss", "04  Stage 2", "05  Stage 2 boss",
        "06  Stage 3", "07  Stage 3 boss", "08  Stage 4", "09  Stage 4 boss", "10  Stage 5",
        "11  Stage 5 boss", "12  Stage 6", "13  Stage 6 boss", "14  Extra stage", "15  Extra boss",
        "16  Ending", "17  Staff roll",
    };

    interface Progress {
        void update(String message);
    }

    // AudioFormat.ENCODING_PCM_24BIT_PACKED / ENCODING_PCM_32BIT (API 31; decoders on older
    // systems never report them).
    private static final int PCM_24BIT_PACKED = 21, PCM_32BIT = 22;

    private MusicReplacer() {}

    /** Where the engine looks (SDL's preference path on Android is the app's files folder). */
    static File dir(Context ctx) {
        return new File(ctx.getFilesDir(), "music");
    }

    static File wav(Context ctx, int track) {
        return new File(dir(ctx), String.format(Locale.ROOT, "th06_%02d.wav", track + 1));
    }

    static File loop(Context ctx, int track) {
        return new File(dir(ctx), String.format(Locale.ROOT, "th06_%02d.pos", track + 1));
    }

    static boolean isReplaced(Context ctx, int track) {
        return wav(ctx, track).isFile();
    }

    static int replacedCount(Context ctx) {
        int n = 0;
        for (int t = 0; t < TRACKS.length; ++t) n += isReplaced(ctx, t) ? 1 : 0;
        return n;
    }

    /** Puts the game's own song back. */
    static void restore(Context ctx, int track) {
        wav(ctx, track).delete();
        loop(ctx, track).delete();
    }

    /** "3:12, loops from 0:00" for a replaced track, or null. */
    static String describe(Context ctx, int track) {
        File w = wav(ctx, track);
        if (!w.isFile()) return null;
        long frames = Math.max(0, (w.length() - 44) / 4);
        long loopStart = 0;
        try (InputStream in = new FileInputStream(loop(ctx, track))) {
            byte[] b = new byte[8];
            if (in.read(b) == 8) loopStart = (b[0] & 0xff) | (b[1] & 0xff) << 8 | (b[2] & 0xff) << 16 | (long) (b[3] & 0xff) << 24;
        } catch (IOException ignored) {
            // No loop file: the whole song loops.
        }
        return String.format(Locale.ROOT, "%s, loops from %s, %.1f MB", time(frames), time(loopStart),
                w.length() / 1048576.0);
    }

    private static String time(long frames) {
        long s = frames / PcmConverter.RATE;
        return String.format(Locale.ROOT, "%d:%02d", s / 60, s % 60);
    }

    /**
     * Decodes {@code source}, converts it and installs it as {@code track}. The loop starts
     * {@code loopStartSeconds} into the song (0: the whole song loops). Returns a summary.
     * Runs on a background thread.
     */
    static String replace(Context ctx, Uri source, int track, double loopStartSeconds, Progress progress)
            throws IOException {
        File folder = dir(ctx);
        if (!folder.isDirectory() && !folder.mkdirs()) throw new IOException("Cannot create " + folder);
        File raw = new File(ctx.getCacheDir(), "music-decode.raw");
        File tmp = new File(folder, "replace.tmp");
        try {
            int[] format = decode(ctx, source, raw, progress);
            progress.update("Converting to WAV (44,100 Hz, stereo)...");
            final long total = raw.length() / (2L * format[1]);
            long frames;
            try (InputStream in = new FileInputStream(raw)) {
                frames = PcmConverter.convert(in, format[0], format[1], tmp, consumed ->
                        progress.update(String.format(Locale.ROOT, "Converting... %d%%", total > 0 ? consumed * 100 / total : 0)));
            }
            long loopStart = Math.round(Math.max(0, loopStartSeconds) * PcmConverter.RATE);
            if (loopStart >= frames) loopStart = 0;
            File wav = wav(ctx, track);
            wav.delete();
            if (!tmp.renameTo(wav)) throw new IOException("Cannot save " + wav);
            PcmConverter.writeLoop(loop(ctx, track), loopStart, frames);
            return describe(ctx, track);
        } finally {
            raw.delete();
            tmp.delete();
        }
    }

    /** Decodes the first audio track to 16-bit PCM in {@code raw}; returns {rate, channels}. */
    private static int[] decode(Context ctx, Uri source, File raw, Progress progress) throws IOException {
        MediaExtractor extractor = new MediaExtractor();
        MediaCodec codec = null;
        try {
            extractor.setDataSource(ctx, source, null);
            MediaFormat format = null;
            for (int i = 0; i < extractor.getTrackCount(); ++i) {
                MediaFormat f = extractor.getTrackFormat(i);
                String mime = f.getString(MediaFormat.KEY_MIME);
                if (mime != null && mime.startsWith("audio/")) {
                    extractor.selectTrack(i);
                    format = f;
                    break;
                }
            }
            if (format == null) throw new IOException("That file has no audio track");
            String mime = format.getString(MediaFormat.KEY_MIME);
            int rate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE);
            int channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
            long durationUs = format.containsKey(MediaFormat.KEY_DURATION) ? format.getLong(MediaFormat.KEY_DURATION) : 0;
            int encoding = AudioFormat.ENCODING_PCM_16BIT;
            codec = MediaCodec.createDecoderByType(mime);
            codec.configure(format, null, null, 0);
            codec.start();
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            byte[] chunk = new byte[1 << 16];
            boolean inputDone = false, outputDone = false;
            int lastPercent = -1;
            try (OutputStream out = new BufferedOutputStream(new FileOutputStream(raw), 1 << 16)) {
                while (!outputDone) {
                    if (!inputDone) {
                        int in = codec.dequeueInputBuffer(10_000);
                        if (in >= 0) {
                            ByteBuffer buffer = codec.getInputBuffer(in);
                            int n = buffer == null ? -1 : extractor.readSampleData(buffer, 0);
                            if (n < 0) {
                                codec.queueInputBuffer(in, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                                inputDone = true;
                            } else {
                                long t = extractor.getSampleTime();
                                codec.queueInputBuffer(in, 0, n, Math.max(0, t), 0);
                                extractor.advance();
                                if (durationUs > 0) {
                                    int percent = (int) Math.min(100, t * 100 / durationUs);
                                    if (percent != lastPercent) {
                                        lastPercent = percent;
                                        progress.update("Decoding... " + percent + "%");
                                    }
                                }
                            }
                        }
                    }
                    int index = codec.dequeueOutputBuffer(info, 10_000);
                    if (index >= 0) {
                        ByteBuffer pcm = codec.getOutputBuffer(index);
                        if (pcm != null && info.size > 0) {
                            pcm.position(info.offset);
                            pcm.limit(info.offset + info.size);
                            write(pcm, encoding, out, chunk);
                        }
                        codec.releaseOutputBuffer(index, false);
                        if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) outputDone = true;
                    } else if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        MediaFormat f = codec.getOutputFormat();
                        if (f.containsKey(MediaFormat.KEY_SAMPLE_RATE)) rate = f.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                        if (f.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) channels = f.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                        if (f.containsKey(MediaFormat.KEY_PCM_ENCODING)) encoding = f.getInteger(MediaFormat.KEY_PCM_ENCODING);
                    }
                }
            }
            if (raw.length() == 0) throw new IOException("Nothing could be decoded from that file");
            return new int[] {rate, channels};
        } catch (IllegalStateException | IllegalArgumentException e) {
            throw new IOException("This file cannot be decoded: " + e.getMessage(), e);
        } finally {
            if (codec != null) {
                try {
                    codec.stop();
                } catch (IllegalStateException ignored) {
                    // Not started.
                }
                codec.release();
            }
            extractor.release();
        }
    }

    /** Writes decoder output as 16-bit little-endian PCM (float and 8/32-bit output converted). */
    private static void write(ByteBuffer pcm, int encoding, OutputStream out, byte[] chunk) throws IOException {
        pcm.order(ByteOrder.nativeOrder());
        if (encoding == AudioFormat.ENCODING_PCM_16BIT) {
            while (pcm.remaining() > 0) {
                int n = Math.min(pcm.remaining(), chunk.length) & ~1;
                if (n == 0) break;
                pcm.get(chunk, 0, n);
                if (ByteOrder.nativeOrder() == ByteOrder.BIG_ENDIAN) {
                    for (int i = 0; i < n; i += 2) {
                        byte t = chunk[i];
                        chunk[i] = chunk[i + 1];
                        chunk[i + 1] = t;
                    }
                }
                out.write(chunk, 0, n);
            }
            return;
        }
        int at = 0;
        while (pcm.remaining() > 0) {
            int v;
            if (encoding == AudioFormat.ENCODING_PCM_FLOAT) {
                if (pcm.remaining() < 4) break;
                v = Math.round(Math.max(-1f, Math.min(1f, pcm.getFloat())) * 32767f);
            } else if (encoding == AudioFormat.ENCODING_PCM_8BIT) {
                v = ((pcm.get() & 0xff) - 128) << 8;
            } else if (encoding == PCM_32BIT) {
                if (pcm.remaining() < 4) break;
                v = pcm.getInt() >> 16;
            } else if (encoding == PCM_24BIT_PACKED) {
                if (pcm.remaining() < 3) break;
                pcm.get();  // the low byte
                v = (pcm.get() & 0xff) | (pcm.get() << 8);
            } else {
                throw new IOException("Unsupported decoder output (encoding " + encoding + ")");
            }
            chunk[at++] = (byte) v;
            chunk[at++] = (byte) (v >> 8);
            if (at == chunk.length) {
                out.write(chunk, 0, at);
                at = 0;
            }
        }
        out.write(chunk, 0, at);
    }
}
