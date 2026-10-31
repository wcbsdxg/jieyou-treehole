#!/usr/bin/env node
/* ============================================================================
 * 解忧树洞 · 求助提示音生成器 (生成 main/assets/help.wav)
 * ----------------------------------------------------------------------------
 * 为什么不用"哔哔"声:
 *   尖锐的方波/纯正弦 + 突然开始结束, 在人耳听来很刺耳, 会吓到本来就紧张的
 *   同学。这里改成"音乐盒/钟琴"音色:
 *     · 大调琶音(C-E-G-C), 听感温暖、明亮、不紧张
 *     · 每个音用多个泛音叠加 + 指数衰减(像敲一下琴键), 不是持续的电子音
 *     · 6ms 软起音 + 结尾渐弱 → 没有"咔哒"爆音
 *     · 整体峰值留余量(约 0.7), 不会破音
 *
 * 用法:
 *   node gen_help_wav.js                     # 默认 chime 音色, 写入 ../../main/assets/help.wav
 *   node gen_help_wav.js soft                # 更舒缓的三音(适合睡前/安静环境)
 *   node gen_help_wav.js harp                # 五音上行(像竖琴)
 *   node gen_help_wav.js chime 我的提示音.wav # 指定输出文件
 *
 * 生成后重新编译烧录:  idf.py build flash
 * ==========================================================================*/
'use strict';
const fs = require('fs');
const path = require('path');

const RATE = 8000;                 // 与固件功放一致(8kHz), 不要改

/* ---------------------------------------------------------------------------
 * 音量与音色说明(按"别吓到同学"来设计的):
 *   peak   : 文件峰值(0~1)。0.26 ≈ -11.7dBFS, 属于"轻轻一响";
 *            电脑上直接双击试听也不会吓人。板子上想更响就按音量+键。
 *   attack : 起音时间(秒)。越长越"软", 25~40ms 完全不会有"叮"的冲击感。
 *   fadeIn : 文件开头 12ms 渐入, 避免第一拍有爆点。
 *   partials: 泛音表。高频泛音越少, 听感越"闷/柔", 越不刺耳。
 * ------------------------------------------------------------------------- */
const PARTIALS_BRIGHT = [   // 明亮(钟琴/八音盒)
    [1.00, 1.00, 1.00],
    [2.00, 0.30, 0.85],
    [3.01, 0.14, 0.65],
    [4.24, 0.06, 0.45],
];
const PARTIALS_MELLOW = [   // 柔和(木琴/风铃, 砍掉高频泛音)
    [1.00, 1.00, 1.00],
    [2.00, 0.16, 0.80],
    [3.00, 0.05, 0.60],
];

/* 音色预设: notes=音高(Hz), step=每音间隔(秒), decay=衰减(秒), dur=单音总长(秒) */
const PRESETS = {
    // 默认: 两个音"叮——咚", 最轻最柔
    gentle: { notes: [523.25, 783.99], amps: [1.0, 0.8], step: 0.44, decay: 1.25, dur: 2.0,
              peak: 0.26, attack: 0.035, partials: PARTIALS_MELLOW },
    // 三音 A4-C#5-E5, 同样很轻
    soft:   { notes: [440.00, 554.37, 659.25], amps: [1.0, 0.85, 0.7], step: 0.46, decay: 1.10, dur: 2.0,
              peak: 0.28, attack: 0.035, partials: PARTIALS_MELLOW },
    // 四音 C5-E5-G5-C6 明亮版(比默认响一点, 仍比最早的"哔哔"柔和得多)
    chime:  { notes: [523.25, 659.25, 783.99, 1046.50], amps: [1.0, 0.9, 0.85, 0.7], step: 0.32, decay: 0.80, dur: 1.8,
              peak: 0.30, attack: 0.028, partials: PARTIALS_BRIGHT },
    // 五音上行, 像竖琴轻拨
    harp:   { notes: [392.00, 523.25, 659.25, 783.99, 1046.50], amps: [0.9, 1.0, 0.85, 0.75, 0.6], step: 0.17, decay: 0.70, dur: 1.5,
              peak: 0.30, attack: 0.022, partials: PARTIALS_BRIGHT },
    // 极轻: 单音"叮", 适合安静的自习环境
    tiny:   { notes: [659.25], amps: [1.0], step: 0, decay: 1.30, dur: 1.6,
              peak: 0.22, attack: 0.040, partials: PARTIALS_MELLOW },
};

function synth(preset) {
    const step = preset.step;
    const totalSec = step * (preset.notes.length - 1) + preset.dur;
    const total = Math.ceil(totalSec * RATE);
    const buf = new Float64Array(total);

    preset.notes.forEach((freq, k) => {
        const start = Math.floor(k * step * RATE);
        const n = Math.floor(preset.dur * RATE);
        const amp = preset.amps[k];
        for (let i = 0; i < n; i++) {
            const idx = start + i;
            if (idx >= total) break;
            const t = i / RATE;
            let s = 0;
            for (const [ratio, pa, dscale] of preset.partials) {
                s += pa * Math.sin(2 * Math.PI * freq * ratio * t) * Math.exp(-t / (preset.decay * dscale));
            }
            const attack = Math.min(1, t / preset.attack);   // 长起音 = 柔和, 无"叮"的冲击
            buf[idx] += s * amp * attack;
        }
    });

    // 归一化到本预设的峰值(默认很轻), 并在开头/结尾做渐入渐出, 杜绝任何"咔哒"或突然冲击
    let peak = 0;
    for (let i = 0; i < total; i++) peak = Math.max(peak, Math.abs(buf[i]));
    const scale = peak > 0 ? (preset.peak * 32767) / peak : 0;
    const fadeOutLen = Math.floor(0.2 * RATE);
    const fadeInLen = Math.floor(0.012 * RATE);      // 开头 12ms 渐入
    const pcm = Buffer.alloc(total * 2);
    for (let i = 0; i < total; i++) {
        let v = buf[i] * scale;
        if (i < fadeInLen) v *= i / fadeInLen;
        if (i > total - fadeOutLen) v *= (total - i) / fadeOutLen;
        pcm.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(v))), i * 2);
    }
    return { pcm, totalSec };
}

function wav(pcm, seconds) {
    const dataLen = pcm.length;
    const head = Buffer.alloc(44);
    head.write('RIFF', 0);
    head.writeUInt32LE(36 + dataLen, 4);
    head.write('WAVE', 8);
    head.write('fmt ', 12);
    head.writeUInt32LE(16, 16);
    head.writeUInt16LE(1, 20);          // PCM
    head.writeUInt16LE(1, 22);          // 单声道
    head.writeUInt32LE(RATE, 24);
    head.writeUInt32LE(RATE * 2, 28);
    head.writeUInt16LE(2, 32);
    head.writeUInt16LE(16, 34);
    head.write('data', 36);
    head.writeUInt32LE(dataLen, 40);
    return Buffer.concat([head, pcm]);
}

/* 音名换算(只用于打印, 让人看懂是哪几个音) */
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
function noteName(freq) {
    const n = Math.round(12 * Math.log2(freq / 440) + 69);      // MIDI 音号
    return NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 1);        // 例: C5
}

/* 听感体检: 峰值/有效值(响度)/跳变是否正常/包络形状 */
function inspect(all, preset) {
    let peak = 0, sumSq = 0, maxJump = 0, prev = 0;
    const env = [];
    let envSum = 0, envCnt = 0;
    const envSpan = Math.floor(RATE * 0.1);                     // 每 100ms 一格
    for (let i = 44; i + 1 < all.length; i += 2) {
        const v = all.readInt16LE(i);
        peak = Math.max(peak, Math.abs(v));
        sumSq += v * v;
        maxJump = Math.max(maxJump, Math.abs(v - prev));
        prev = v;
        envSum += v * v; envCnt++;
        if (envCnt >= envSpan) {
            env.push(Math.sqrt(envSum / envCnt));
            envSum = 0; envCnt = 0;
        }
    }
    const rms = Math.sqrt(sumSq / ((all.length - 44) / 2));
    // 正常斜率上限 = 2π·最高音频率·峰值 / 采样率; 实测跳变接近它就说明是正常波形, 没有爆音
    const fmax = Math.max(...preset.notes) * 4.24;               // 最高泛音
    const slopeLimit = 2 * Math.PI * fmax * peak / RATE;
    return {
        peak, rms,
        peakDb: (20 * Math.log10(peak / 32768)).toFixed(1),
        rmsDb: (20 * Math.log10(rms / 32768)).toFixed(1),
        maxJump, slopeLimit,
        env,
    };
}

function drawEnvelope(env) {
    const max = Math.max(...env, 1);
    const chars = ' ▁▂▃▄▅▆▇█';
    return env.map(v => chars[Math.min(8, Math.round((v / max) * 8))]).join('');
}

// 用法: node gen_help_wav.js [音色] [输出文件] [峰值0~1]
const name = (process.argv[2] || 'gentle').toLowerCase();
const preset = PRESETS[name];
if (!preset) {
    console.error(`未知音色 "${name}"。可选: ${Object.keys(PRESETS).join(' / ')}`);
    process.exit(1);
}
const out = process.argv[3] || path.join(__dirname, '..', 'main', 'assets', 'help.wav');
if (process.argv[4] !== undefined) {
    const p = parseFloat(process.argv[4]);
    if (!(p > 0 && p <= 1)) {
        console.error('峰值要在 0~1 之间, 例如 0.2');
        process.exit(1);
    }
    preset.peak = p;                 // 命令行临时覆盖音量
}

const { pcm, totalSec } = synth(preset);
const data = wav(pcm, totalSec);
fs.writeFileSync(out, data);
const s = inspect(data, preset);
const jumps = s.maxJump <= s.slopeLimit * 1.05;

console.log(`音色      : ${name}  (${preset.notes.map(f => noteName(f)).join(' → ')}  ${preset.notes.map(f => f.toFixed(0)).join('-')}Hz)`);
console.log(`输出文件  : ${out}`);
console.log(`规格      : ${RATE}Hz / 16bit / 单声道 / ${totalSec.toFixed(2)} 秒 / ${data.length} 字节`);
console.log(`音量      : 峰值 ${s.peak} (${s.peakDb} dBFS), 响度 ${s.rmsDb} dBFS  ${s.peakDb > -6 ? '← 偏响!' : '← 温和, 不会吓人'}`);
console.log(`起音      : ${(preset.attack * 1000).toFixed(0)}ms 软起音 + 开头渐入, 结尾自然渐弱`);
console.log(`跳变体检  : 实测 ${s.maxJump} / 理论上限 ${s.slopeLimit.toFixed(0)}  → ${jumps ? '正常, 无爆音' : '偏大, 请检查'}`);
console.log(`音量包络  : |${drawEnvelope(s.env)}|  (每格 0.1 秒, 敲击后自然衰减)`);
console.log('提示: 电脑试听请先把系统音量调到 30% 左右; 板子上想更响就按音量+ 键。');
console.log('生成完成, 重新编译烧录: idf.py build flash');
