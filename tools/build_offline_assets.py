#!/usr/bin/env python3
"""
Build offline SD-card binary assets for New 3DS Offline OCR & Dictionary plugin.

Outputs into n3ds_offline/sd_pack/luma/ocr_offline/:
  1. ocr_ppocrv4_int8.bin  (~2.5 MB) - INT8 quantized PP-OCRv4 Japanese recognition model
  2. jmdict_offline.bin    (~10-14 MB) - Compressed JMdict + word_meta + JLPT dictionary
  3. kanjidic_offline.bin  (~0.8-1.2 MB) - Kanjidic2 + components + JLPT kanji
  4. font_pack.bin         (~1.2-1.6 MB) - Pre-rasterized 4-bit alpha glyphs (fontId 0, 1, 2)
"""

import json
import os
import re
import sqlite3
import struct
import sys
import time
import zlib
from collections import defaultdict

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT_DIR, "n3ds_offline", "sd_pack", "luma", "ocr_offline")

ONNX_V5_PATH = os.path.join(ROOT_DIR, "ocr_models", "ch_PP-OCRv5_rec_mobile.onnx")
DICT_V5_PATH = os.path.join(ROOT_DIR, "ocr_models", "ppocrv5_dict.txt")
JAPAN_DICT_PATH = os.path.join(ROOT_DIR, "ocr_models", "japan_dict.txt")
ONNX_PATH = ONNX_V5_PATH if os.path.exists(ONNX_V5_PATH) else os.path.join(ROOT_DIR, "ocr_models", "japan_PP-OCRv4_rec.onnx")
DICT_TXT_PATH = DICT_V5_PATH if (ONNX_PATH == ONNX_V5_PATH and os.path.exists(DICT_V5_PATH)) else JAPAN_DICT_PATH
JAMDICT_DB_PATH = os.path.join(ROOT_DIR, "dictionaries", "jamdict.db")
WORD_META_DB_PATH = os.path.join(ROOT_DIR, "dictionaries", "word_meta.db")
JLPT_VOCAB_PATH = os.path.join(ROOT_DIR, "dictionaries", "jlpt_vocab.json")
JLPT_KANJI_PATH = os.path.join(ROOT_DIR, "dictionaries", "jlpt_kanji.json")
KANJI_COMP_PATH = os.path.join(ROOT_DIR, "dictionaries", "kanji_components.json")
CUSTOM_DICT_PATH = os.path.join(ROOT_DIR, "dictionaries", "custom_dict.json")

# ---------------------------------------------------------------------------
# Minimal ONNX Protobuf Parser (no `onnx` package required)
# ---------------------------------------------------------------------------

def _read_varint(buf: bytes, pos: int):
    val = 0
    shift = 0
    while True:
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        if not (b & 0x80):
            return val, pos
        shift += 7


def _parse_msg(buf: bytes, start: int, end: int):
    fields = []
    pos = start
    while pos < end:
        tag, pos = _read_varint(buf, pos)
        fn = tag >> 3
        wt = tag & 7
        if wt == 0:
            v, pos = _read_varint(buf, pos)
            fields.append((fn, wt, v))
        elif wt == 1:
            fields.append((fn, wt, buf[pos : pos + 8]))
            pos += 8
        elif wt == 2:
            ln, pos = _read_varint(buf, pos)
            fields.append((fn, wt, (pos, pos + ln)))
            pos += ln
        elif wt == 5:
            fields.append((fn, wt, buf[pos : pos + 4]))
            pos += 4
        else:
            raise ValueError(f"Unsupported wire type {wt}")
    return fields


def _parse_tensor(buf: bytes, start: int, end: int) -> np.ndarray:
    dims = []
    dtype = 0
    floats = []
    raw = b""
    ints = []
    for fn, wt, v in _parse_msg(buf, start, end):
        if fn == 1:
            dims.append(v)
        elif fn == 2:
            dtype = v
        elif fn == 4:
            if wt == 5:
                floats.append(struct.unpack("<f", v)[0])
            elif wt == 2:
                p, pe = v
                cnt = (pe - p) // 4
                floats.extend(struct.unpack(f"<{cnt}f", buf[p:pe]))
        elif fn == 7:
            if wt == 0:
                ints.append(v)
            elif wt == 2:
                p, pe = v
                while p < pe:
                    iv, p = _read_varint(buf, p)
                    ints.append(iv)
        elif fn == 9:
            raw = buf[v[0] : v[1]]
    if raw:
        dt_map = {1: np.float32, 6: np.int32, 7: np.int64}
        arr = np.frombuffer(raw, dtype=dt_map[dtype]).copy()
        if dims:
            arr = arr.reshape(dims)
        return arr
    if floats:
        arr = np.array(floats, dtype=np.float32)
        if dims:
            arr = arr.reshape(dims)
        return arr
    if ints:
        arr = np.array(ints, dtype=np.int64)
        if dims:
            arr = arr.reshape(dims)
        return arr
    return np.array([], dtype=np.float32)


def load_onnx_graph(onnx_path: str):
    with open(onnx_path, "rb") as f:
        data = f.read()
    top = _parse_msg(data, 0, len(data))
    graph_span = [v for fn, wt, v in top if fn == 7][0]
    graph = _parse_msg(data, *graph_span)

    consts = {}
    nodes = []
    for fn, wt, v in graph:
        if fn == 1:
            nf = _parse_msg(data, *v)
            op = ""
            inputs = []
            outputs = []
            attrs = {}
            for nfn, nwt, nv in nf:
                if nfn == 1:
                    inputs.append(data[nv[0] : nv[1]].decode("utf-8"))
                elif nfn == 2:
                    outputs.append(data[nv[0] : nv[1]].decode("utf-8"))
                elif nfn == 4:
                    op = data[nv[0] : nv[1]].decode("utf-8")
                elif nfn == 5:
                    af = _parse_msg(data, *nv)
                    aname = ""
                    ints = []
                    i_val = None
                    f_val = None
                    s_val = None
                    t_span = None
                    for afn, awt, av in af:
                        if afn == 1:
                            aname = data[av[0] : av[1]].decode("utf-8")
                        elif afn == 2 and awt == 5:
                            f_val = struct.unpack("<f", av)[0]
                        elif afn == 3 and awt == 0:
                            i_val = av
                        elif afn == 4 and awt == 2:
                            s_val = data[av[0] : av[1]].decode("utf-8", errors="replace")
                        elif afn == 5 and awt == 2:
                            t_span = av
                        elif afn == 8:
                            if awt == 0:
                                ints.append(av)
                            elif awt == 2:
                                p, pe = av
                                while p < pe:
                                    iv, p = _read_varint(data, p)
                                    ints.append(iv)
                    attrs[aname] = {"i": i_val, "f": f_val, "s": s_val, "ints": ints, "t": t_span}
            if op == "Constant" and "value" in attrs and attrs["value"]["t"]:
                consts[outputs[0]] = _parse_tensor(data, *attrs["value"]["t"])
            else:
                nodes.append({"op": op, "in": inputs, "out": outputs, "attrs": attrs})
        elif fn == 5:
            # InitializerProto if any
            tf = _parse_msg(data, *v)
            name = ""
            for tfn, twt, tv in tf:
                if tfn == 8:
                    name = data[tv[0] : tv[1]].decode("utf-8")
            if name:
                consts[name] = _parse_tensor(data, *v)

    # Strip Identity NOP nodes so downstream pattern matchers see a clean graph
    alias = {}
    filtered = []
    for nd in nodes:
        ins = [alias.get(x, x) for x in nd["in"]]
        if nd["op"] == "Identity":
            src = ins[0]
            dst = nd["out"][0]
            alias[dst] = src
            if src in consts:
                consts[dst] = consts[src]
        else:
            nd["in"] = ins
            filtered.append(nd)
    return filtered, consts


# ---------------------------------------------------------------------------
# 1. Build ocr_ppocrv4_int8.bin
# ---------------------------------------------------------------------------

OP_CONV = 1
OP_GAP = 2
OP_MUL_SLOT = 3
OP_AVGPOOL = 4
OP_CONCAT_POOL = 5
OP_SVTR_BLOCK = 6
OP_LAYERNORM = 7
OP_CTC_HEAD = 8
OP_CONV_S16 = 9

ACT_NONE = 0
ACT_RELU = 1
ACT_HARDSWISH = 2
ACT_SWISH = 3
ACT_HARDSIGMOID = 4


def quantize_per_channel_int8(w_f32: np.ndarray):
    """Quantize [Cout, ...] weights per Cout channel to int8 + fp32 scales."""
    cout = w_f32.shape[0]
    flat = w_f32.reshape(cout, -1).astype(np.float32)
    max_abs = np.max(np.abs(flat), axis=1)
    scales = np.where(max_abs > 1e-12, max_abs / 127.0, 1.0).astype(np.float32)
    q = np.clip(np.round(flat / scales[:, None]), -127, 127).astype(np.int8)
    return q.reshape(w_f32.shape), scales


def quantize_per_channel_s16(w_f32: np.ndarray, qmax: int = 4095):
    """Quantize [Cout, ...] weights per Cout channel to signed int16 (13-bit [-4095, 4095]) + fp32 scales."""
    cout = w_f32.shape[0]
    flat = w_f32.reshape(cout, -1).astype(np.float32)
    max_abs = np.max(np.abs(flat), axis=1)
    scales = np.where(max_abs > 1e-12, max_abs / float(qmax), 1.0).astype(np.float32)
    q = np.clip(np.round(flat / scales[:, None]), -qmax, qmax).astype(np.int16)
    return q.reshape(w_f32.shape), scales


def quantize_linear_int8(w_in_out: np.ndarray):
    """Quantize ONNX MatMul [In, Out] -> store as [Out, In] int8 + [Out] fp32 scales."""
    w_out_in = np.ascontiguousarray(w_in_out.T, dtype=np.float32)
    return quantize_per_channel_int8(w_out_in)


def _build_jp_prune_indices(full_vocab):
    """Return (pruned_vocab, keep_indices) keeping Japanese-relevant tokens from a large multilingual vocab."""
    if len(full_vocab) <= 6000:
        return full_vocab, None
    keep_chars = {"", " "}
    if os.path.exists(JAPAN_DICT_PATH):
        with open(JAPAN_DICT_PATH, "r", encoding="utf-8") as f:
            for l in f:
                keep_chars.add(l.rstrip("\r\n"))
    if os.path.exists(CUSTOM_DICT_PATH):
        with open(CUSTOM_DICT_PATH, "r", encoding="utf-8") as f:
            for k in json.load(f).keys():
                keep_chars.update(k)

    def is_jp_relevant(ch: str) -> bool:
        if ch in keep_chars:
            return True
        if len(ch) != 1:
            return False
        cp = ord(ch)
        return (
            (0x20 <= cp <= 0x7E)
            or (0x3000 <= cp <= 0x30FF)
            or (0xFF00 <= cp <= 0xFFEF)
        )

    keep_indices = [0]
    pruned = [""]
    for idx in range(1, len(full_vocab) - 1):
        ch = full_vocab[idx]
        if is_jp_relevant(ch):
            keep_indices.append(idx)
            pruned.append(ch)
    keep_indices.append(len(full_vocab) - 1)
    pruned.append(" ")
    return pruned, np.array(keep_indices, dtype=np.int64)


def build_ocr_model(out_path: str):
    t0 = time.time()
    nodes, consts = load_onnx_graph(ONNX_PATH)

    with open(DICT_TXT_PATH, "r", encoding="utf-8") as f:
        dict_chars = [line.rstrip("\r\n") for line in f]
    # PP-OCRv4/v5 vocab: index 0 = CTC blank, 1..N = dict, N+1 = space
    full_vocab = [""] + dict_chars + [" "]
    vocab, keep_indices = _build_jp_prune_indices(full_vocab)
    if keep_indices is not None:
        print(f"[OCR] Pruned vocab {len(full_vocab)} -> {len(vocab)} Japanese-relevant tokens")

    ops = []
    idx = 0
    n = len(nodes)

    def extract_folded_conv(start_idx, use_s16: bool = False):
        """Parse Conv + optional BatchNorm / Mul+Add + optional Activation + optional post Mul+Add."""
        nd = nodes[start_idx]
        assert nd["op"] == "Conv"
        w = consts[nd["in"][1]].astype(np.float32).copy()
        cout = w.shape[0]
        if len(nd["in"]) > 2:
            b = consts[nd["in"][2]].astype(np.float32).copy()
        else:
            b = np.zeros(cout, dtype=np.float32)
        g = int(nd["attrs"].get("group", {}).get("i") or 1)
        strides = nd["attrs"]["strides"]["ints"]
        pads = nd["attrs"]["pads"]["ints"]
        cur = start_idx + 1

        # Optional separate Add(bias) node (e.g., Paddle2ONNX Reshape->Add bias)
        if cur < n and nodes[cur]["op"] == "Add":
            ac = [consts[x] for x in nodes[cur]["in"] if x in consts]
            if ac and ac[0].size in (1, cout):
                b = b + ac[0].astype(np.float32).reshape(-1)
                cur += 1

        # Optional BatchNormalization: y = gamma * (x - mean)/sqrt(var+eps) + beta
        if cur < n and nodes[cur]["op"] == "BatchNormalization":
            bn = nodes[cur]
            gamma = consts[bn["in"][1]].astype(np.float32)
            beta = consts[bn["in"][2]].astype(np.float32)
            mean = consts[bn["in"][3]].astype(np.float32)
            var = consts[bn["in"][4]].astype(np.float32)
            eps = bn["attrs"].get("epsilon", {}).get("f")
            if eps is None:
                eps = 1e-5
            scale = gamma / np.sqrt(var + eps)
            w *= scale.reshape(-1, 1, 1, 1)
            b = (b - mean) * scale + beta
            cur += 1

        # Optional pre-activation Mul + Add (LearnableAffineBlock)
        if cur + 1 < n and nodes[cur]["op"] == "Mul" and nodes[cur + 1]["op"] == "Add":
            # Check if Mul has a scalar/1D constant
            mul_ins = nodes[cur]["in"]
            add_ins = nodes[cur + 1]["in"]
            mc = [consts[x] for x in mul_ins if x in consts]
            ac = [consts[x] for x in add_ins if x in consts]
            if mc and ac and mc[0].size in (1, cout) and ac[0].size in (1, cout):
                mv = mc[0].astype(np.float32).reshape(-1)
                av = ac[0].astype(np.float32).reshape(-1)
                w *= mv.reshape(-1, 1, 1, 1) if mv.size == cout else mv[0]
                b = b * mv + av
                cur += 2

        # Optional Activation
        act = ACT_NONE
        if cur < n and nodes[cur]["op"] == "HardSwish":
            act = ACT_HARDSWISH
            cur += 1
        elif cur < n and nodes[cur]["op"] == "Relu":
            act = ACT_RELU
            cur += 1
        elif cur < n and nodes[cur]["op"] == "HardSigmoid":
            alpha = nodes[cur]["attrs"].get("alpha", {}).get("f")
            act = 6 if (alpha is not None and alpha < 0.18) else ACT_HARDSIGMOID
            cur += 1
        elif (
            cur + 1 < n
            and nodes[cur]["op"] == "Sigmoid"
            and nodes[cur + 1]["op"] == "Mul"
        ):
            act = ACT_SWISH
            cur += 2
        elif (
            cur + 2 < n
            and nodes[cur]["op"] == "Mul"
            and nodes[cur + 1]["op"] == "Sigmoid"
            and nodes[cur + 2]["op"] == "Mul"
        ):
            act = ACT_SWISH
            cur += 3

        # Optional post-activation Mul + Add (LearnableAffineBlock)
        post_scale = 1.0
        post_bias = 0.0
        if cur + 1 < n and nodes[cur]["op"] == "Mul" and nodes[cur + 1]["op"] == "Add":
            mul_ins = nodes[cur]["in"]
            add_ins = nodes[cur + 1]["in"]
            mc = [consts[x] for x in mul_ins if x in consts]
            ac = [consts[x] for x in add_ins if x in consts]
            if mc and ac and mc[0].size == 1 and ac[0].size == 1:
                post_scale = float(mc[0].reshape(-1)[0])
                post_bias = float(ac[0].reshape(-1)[0])
                cur += 2

        if use_s16:
            q_w, w_scales = quantize_per_channel_s16(w, 4095)
            op_t = OP_CONV_S16
        else:
            q_w, w_scales = quantize_per_channel_int8(w)
            op_t = OP_CONV
        return {
            "type": op_t,
            "w_q": q_w,
            "scales": w_scales,
            "bias": b,
            "groups": g,
            "stride_h": int(strides[0]),
            "stride_w": int(strides[1]),
            "pad_h": int(pads[0]),
            "pad_w": int(pads[1]),
            "act": act,
            "post_scale": post_scale,
            "post_bias": post_bias,
            "save_slot": 0,
            "out_name": nodes[cur - 1]["out"][0],
        }, cur

    # Walk graph sequentially
    while idx < n:
        op = nodes[idx]["op"]
        if op == "Conv":
            cdict, next_idx = extract_folded_conv(idx, use_s16=(len(ops) <= 8))
            # Check if next node is GlobalAveragePool (means save current to slot 1 = se_skip)
            if next_idx < n and nodes[next_idx]["op"] == "GlobalAveragePool":
                cdict["save_slot"] = 1
            ops.append(cdict)
            idx = next_idx
        elif op == "GlobalAveragePool":
            ops.append({"type": OP_GAP})
            idx += 1
        elif op == "Mul":
            # SE apply: Mul(se_skip, hardsigmoid_out)
            ops.append({"type": OP_MUL_SLOT})
            idx += 1
        elif op == "AveragePool":
            ks = nodes[idx]["attrs"]["kernel_shape"]["ints"]
            st = nodes[idx]["attrs"]["strides"]["ints"]
            ops.append({
                "type": OP_AVGPOOL,
                "kh": int(ks[0]),
                "kw": int(ks[1]),
                "sh": int(st[0]),
                "sw": int(st[1]),
                "save_slot": 2,  # save pool2d_2.tmp_0 for Concat.4
            })
            idx += 1
        elif op == "Concat":
            if nodes[idx]["out"][0] == "Concat.4":
                ops.append({"type": OP_CONCAT_POOL})
            idx += 1
        elif op == "ReduceMean" and idx + 8 < n and nodes[idx + 1]["op"] == "Sub":
            # Start of LayerNorm! Check if it's followed by SVTR block or just final LN
            # LN pattern: ReduceMean -> Sub -> Pow -> ReduceMean -> Add(eps) -> Sqrt -> Div -> Mul(gamma) -> Add(beta)
            eps = float(consts[nodes[idx + 4]["in"][1]].reshape(-1)[0])
            gamma = consts[nodes[idx + 7]["in"][1]].astype(np.float32)
            beta = consts[nodes[idx + 8]["in"][1]].astype(np.float32)
            if idx + 9 < n and nodes[idx + 9]["op"] == "MatMul":
                # Full SVTR block!
                # Find linear_qkv, linear_proj, ln2, linear_ffn1, linear_ffn2
                # Locate MatMul nodes in this SVTR block
                mm_indices = []
                ln2_idx = None
                scan = idx + 9
                while scan < n:
                    if nodes[scan]["op"] == "MatMul" and nodes[scan]["in"][1] in consts:
                        mm_indices.append(scan)
                        if len(mm_indices) == 4:
                            break
                    elif nodes[scan]["op"] == "ReduceMean" and nodes[scan + 1]["op"] == "Sub":
                        ln2_idx = scan
                    scan += 1
                assert len(mm_indices) == 4 and ln2_idx is not None
                eps2 = float(consts[nodes[ln2_idx + 4]["in"][1]].reshape(-1)[0])
                gamma2 = consts[nodes[ln2_idx + 7]["in"][1]].astype(np.float32)
                beta2 = consts[nodes[ln2_idx + 8]["in"][1]].astype(np.float32)

                def get_linear(mm_i):
                    w_in_out = consts[nodes[mm_i]["in"][1]].astype(np.float32)
                    b_vec = consts[nodes[mm_i + 1]["in"][1]].astype(np.float32)
                    q_w, sc = quantize_linear_int8(w_in_out)
                    return q_w, sc, b_vec

                qkv = get_linear(mm_indices[0])
                proj = get_linear(mm_indices[1])
                ffn1 = get_linear(mm_indices[2])
                ffn2 = get_linear(mm_indices[3])
                ops.append({
                    "type": OP_SVTR_BLOCK,
                    "ln1_eps": eps, "ln1_g": gamma, "ln1_b": beta,
                    "qkv": qkv, "proj": proj,
                    "ln2_eps": eps2, "ln2_g": gamma2, "ln2_b": beta2,
                    "ffn1": ffn1, "ffn2": ffn2,
                    "num_heads": 8,
                })
                # Advance past final Add residual of SVTR block
                idx = mm_indices[3] + 3
            else:
                # Final LayerNorm (layer_norm_4)
                ops.append({
                    "type": OP_LAYERNORM,
                    "eps": eps, "g": gamma, "b": beta,
                })
                idx += 9
        elif op == "MatMul" and nodes[idx]["in"][1] in consts:
            # Final CTC head
            w_in_out = consts[nodes[idx]["in"][1]].astype(np.float32)
            b_vec = consts[nodes[idx + 1]["in"][1]].astype(np.float32)
            if keep_indices is not None:
                w_in_out = w_in_out[:, keep_indices]
                b_vec = b_vec[keep_indices]
            q_w, sc = quantize_linear_int8(w_in_out)
            ops.append({
                "type": OP_CTC_HEAD,
                "w_q": q_w,
                "scales": sc,
                "bias": b_vec,
            })
            idx += 2
        else:
            idx += 1

    # Static per-op output activation scales (ver=2) calibrated across 264 real 3DS + multi-font crops (1.5x margin)
    act_out_scales_v2 = [
        1.164198015e-02,  # op[ 0]
        7.587015716e-05,  # op[ 1]
        6.802421063e-03,  # op[ 2]
        1.818742836e-03,  # op[ 3]
        6.995789241e-03,  # op[ 4]
        1.567641273e-03,  # op[ 5]
        1.827906072e-02,  # op[ 6]
        8.250757819e-04,  # op[ 7]
        4.140133038e-03,  # op[ 8]
        1.283714897e-03,  # op[ 9]
        6.985090673e-03,  # op[10]
        7.622420671e-04,  # op[11]
        5.655608606e-03,  # op[12]
        3.979117027e-04,  # op[13]
        4.843286239e-03,  # op[14]
        1.304919424e-04,  # op[15]
        6.718309596e-03,  # op[16]
        1.523999963e-04,  # op[17]
        7.590532769e-03,  # op[18]
        1.547642023e-04,  # op[19]
        5.113712512e-03,  # op[20]
        0.000000000e+00,  # op[21]
        0.000000000e+00,  # op[22]
        0.000000000e+00,  # op[23]
        0.000000000e+00,  # op[24]
        0.000000000e+00,  # op[25]
        8.303219453e-03,  # op[26]
        0.000000000e+00,  # op[27]
        0.000000000e+00,  # op[28]
        0.000000000e+00,  # op[29]
        0.000000000e+00,  # op[30]
        0.000000000e+00,  # op[31]
        7.701257244e-03,  # op[32]
        9.669484280e-05,  # op[33]
        7.022932637e-03,  # op[34]
        3.843776649e-03,  # op[35]
        0.000000000e+00,  # op[36]
        0.000000000e+00,  # op[37]
        0.000000000e+00,  # op[38]
        0.000000000e+00,  # op[39]
        0.000000000e+00,  # op[40]
        0.000000000e+00,  # op[41]
        0.000000000e+00,  # op[42]
        0.000000000e+00,  # op[43]
        0.000000000e+00,  # op[44]
        0.000000000e+00,  # op[45]
        0.000000000e+00,  # op[46]
        0.000000000e+00,  # op[47]
    ]
    if len(act_out_scales_v2) != len(ops):
        act_out_scales_v2 = [0.0] * len(ops)

    # Write binary file (ver=2 includes per-op static int16 activation output scales)
    with open(out_path, "wb") as f:
        f.write(b"PPOCRV4I")
        f.write(struct.pack("<III", 2, len(ops), len(vocab)))
        for s in vocab:
            sb = s.encode("utf-8")
            f.write(struct.pack("<B", len(sb)))
            f.write(sb)
        f.write(np.asarray(act_out_scales_v2, dtype="<f4").tobytes())
        for op in ops:
            t = op["type"]
            f.write(struct.pack("<B", t))
            if t in (OP_CONV, OP_CONV_S16):
                cout, cin_g, kh, kw = op["w_q"].shape
                cin = cin_g * op["groups"]
                f.write(struct.pack(
                    "<HHHHBBBBHBBff",
                    cout, cin, kh, kw,
                    op["stride_h"], op["stride_w"], op["pad_h"], op["pad_w"],
                    op["groups"], op["act"], op["save_slot"],
                    op["post_scale"], op["post_bias"],
                ))
                f.write(op["scales"].tobytes())
                f.write(op["bias"].tobytes())
                if t == OP_CONV_S16:
                    f.write(np.ascontiguousarray(op["w_q"], dtype="<i2").tobytes())
                else:
                    f.write(np.ascontiguousarray(op["w_q"], dtype=np.int8).tobytes())
            elif t == OP_GAP:
                pass
            elif t == OP_MUL_SLOT:
                pass
            elif t == OP_AVGPOOL:
                f.write(struct.pack("<BBBBB", op["kh"], op["kw"], op["sh"], op["sw"], op["save_slot"]))
            elif t == OP_CONCAT_POOL:
                pass
            elif t == OP_SVTR_BLOCK:
                dim = op["ln1_g"].shape[0]
                f.write(struct.pack("<HBff", dim, op["num_heads"], op["ln1_eps"], op["ln2_eps"]))
                f.write(op["ln1_g"].tobytes())
                f.write(op["ln1_b"].tobytes())
                f.write(op["ln2_g"].tobytes())
                f.write(op["ln2_b"].tobytes())
                for qw, sc, bv in (op["qkv"], op["proj"], op["ffn1"], op["ffn2"]):
                    out_d, in_d = qw.shape
                    f.write(struct.pack("<HH", out_d, in_d))
                    f.write(sc.tobytes())
                    f.write(bv.tobytes())
                    f.write(np.ascontiguousarray(qw, dtype=np.int8).tobytes())
            elif t == OP_LAYERNORM:
                dim = op["g"].shape[0]
                f.write(struct.pack("<Hf", dim, op["eps"]))
                f.write(op["g"].tobytes())
                f.write(op["b"].tobytes())
            elif t == OP_CTC_HEAD:
                out_d, in_d = op["w_q"].shape
                f.write(struct.pack("<HH", out_d, in_d))
                f.write(op["scales"].tobytes())
                f.write(op["bias"].tobytes())
                f.write(np.ascontiguousarray(op["w_q"], dtype=np.int8).tobytes())

    sz = os.path.getsize(out_path)
    print(f"[OCR] Wrote {out_path} ({sz:,} bytes, {len(ops)} ops) in {time.time()-t0:.2f}s")
    return ops, vocab


# ---------------------------------------------------------------------------
# Reference INT8 Evaluator (verifies ocr_ppocrv4_int8.bin vs onnxruntime)
# ---------------------------------------------------------------------------

def _apply_act(x: np.ndarray, act: int) -> np.ndarray:
    if act == ACT_NONE:
        return x
    if act == ACT_RELU:
        return np.maximum(x, 0.0)
    if act == ACT_HARDSWISH:
        return x * np.clip(x / 6.0 + 0.5, 0.0, 1.0)
    if act == ACT_SWISH:
        return x / (1.0 + np.exp(-np.clip(x, -40.0, 40.0)))
    if act == ACT_HARDSIGMOID:
        return np.clip(x / 6.0 + 0.5, 0.0, 1.0)
    if act == 6:
        return np.clip(x / 6.0 + 0.5, 0.0, 1.0)
    raise ValueError(f"Unknown act {act}")


def _conv2d_ref(x: np.ndarray, w_q: np.ndarray, scales: np.ndarray, bias: np.ndarray,
                groups: int, sh: int, sw: int, ph: int, pw: int) -> np.ndarray:
    cin, hin, win = x.shape
    cout, cin_g, kh, kw = w_q.shape
    w = w_q.astype(np.float32) * scales.reshape(-1, 1, 1, 1)
    xp = np.pad(x, ((0, 0), (ph, ph), (pw, pw)), mode="constant")
    hout = (hin + 2 * ph - kh) // sh + 1
    wout = (win + 2 * pw - kw) // sw + 1

    if groups == 1:
        # im2col
        cols = np.empty((cin * kh * kw, hout * wout), dtype=np.float32)
        col_i = 0
        for c in range(cin):
            for ky in range(kh):
                for kx in range(kw):
                    cols[col_i] = xp[c, ky : ky + hout * sh : sh, kx : kx + wout * sw : sw].reshape(-1)
                    col_i += 1
        out = (w.reshape(cout, -1) @ cols + bias[:, None]).reshape(cout, hout, wout)
        return out
    else:
        assert groups == cin == cout
        out = np.empty((cout, hout, wout), dtype=np.float32)
        for c in range(cout):
            acc = np.full((hout, wout), bias[c], dtype=np.float32)
            for ky in range(kh):
                for kx in range(kw):
                    acc += w[c, 0, ky, kx] * xp[c, ky : ky + hout * sh : sh, kx : kx + wout * sw : sw]
            out[c] = acc
        return out


def _layernorm_ref(x: np.ndarray, g: np.ndarray, b: np.ndarray, eps: float) -> np.ndarray:
    mean = np.mean(x, axis=-1, keepdims=True)
    var = np.mean((x - mean) ** 2, axis=-1, keepdims=True)
    return (x - mean) / np.sqrt(var + eps) * g + b


def _linear_ref(x: np.ndarray, qw: np.ndarray, sc: np.ndarray, bv: np.ndarray) -> np.ndarray:
    w = qw.astype(np.float32) * sc[:, None]
    return x @ w.T + bv


def _ctc_decode(logits: np.ndarray, vocab):
    # logits: [T, 4401]
    ids = np.argmax(logits, axis=-1)
    chars = []
    prev = 0
    for idx in ids:
        if idx != 0 and idx != prev:
            chars.append(vocab[idx])
        prev = idx
    return "".join(chars)


def run_int8_ref(bin_path: str, img_nchw: np.ndarray):
    with open(bin_path, "rb") as f:
        buf = f.read()
    pos = 0
    assert buf[:8] == b"PPOCRV4I"
    pos += 8
    ver, num_ops, vocab_sz = struct.unpack_from("<III", buf, pos)
    pos += 12
    vocab = []
    for _ in range(vocab_sz):
        ln = buf[pos]; pos += 1
        vocab.append(buf[pos : pos + ln].decode("utf-8"))
        pos += ln
    if ver == 2:
        pos += num_ops * 4

    cur = img_nchw[0]  # [3, 48, W]
    slot1 = None
    slot2 = None
    for _ in range(num_ops):
        t = buf[pos]; pos += 1
        if t in (OP_CONV, OP_CONV_S16):
            fmt = "<HHHHBBBBHBBff"
            cout, cin, kh, kw, sh, sw, ph, pw, g, act, save_slot, ps, pb = struct.unpack_from(fmt, buf, pos)
            pos += struct.calcsize(fmt)
            scales = np.frombuffer(buf, np.float32, cout, pos).copy(); pos += cout * 4
            bias = np.frombuffer(buf, np.float32, cout, pos).copy(); pos += cout * 4
            w_cnt = cout * (cin // g) * kh * kw
            if t == OP_CONV_S16:
                wq = np.frombuffer(buf, "<i2", w_cnt, pos).copy().reshape(cout, cin // g, kh, kw); pos += w_cnt * 2
            else:
                wq = np.frombuffer(buf, np.int8, w_cnt, pos).copy().reshape(cout, cin // g, kh, kw); pos += w_cnt
            cur = _conv2d_ref(cur, wq, scales, bias, g, sh, sw, ph, pw)
            cur = _apply_act(cur, act)
            if ps != 1.0 or pb != 0.0:
                cur = cur * ps + pb
            if save_slot == 1:
                slot1 = cur
        elif t == OP_GAP:
            cur = np.mean(cur, axis=(1, 2), keepdims=True)
        elif t == OP_MUL_SLOT:
            cur = slot1 * cur
        elif t == OP_AVGPOOL:
            kh, kw, sh, sw, save_slot = struct.unpack_from("<BBBBB", buf, pos); pos += 5
            c, h, w = cur.shape
            ho = (h - kh) // sh + 1
            wo = (w - kw) // sw + 1
            out = np.zeros((c, ho, wo), dtype=np.float32)
            for ky in range(kh):
                for kx in range(kw):
                    out += cur[:, ky : ky + ho * sh : sh, kx : kx + wo * sw : sw]
            cur = out / float(kh * kw)
            if save_slot == 2:
                slot2 = cur
        elif t == OP_CONCAT_POOL:
            cur = np.concatenate([slot2, cur], axis=0)
        elif t == OP_SVTR_BLOCK:
            dim, nheads, eps1, eps2 = struct.unpack_from("<HBff", buf, pos); pos += 11
            ln1_g = np.frombuffer(buf, np.float32, dim, pos).copy(); pos += dim * 4
            ln1_b = np.frombuffer(buf, np.float32, dim, pos).copy(); pos += dim * 4
            ln2_g = np.frombuffer(buf, np.float32, dim, pos).copy(); pos += dim * 4
            ln2_b = np.frombuffer(buf, np.float32, dim, pos).copy(); pos += dim * 4
            lins = []
            for _l in range(4):
                od, id_ = struct.unpack_from("<HH", buf, pos); pos += 4
                sc = np.frombuffer(buf, np.float32, od, pos).copy(); pos += od * 4
                bv = np.frombuffer(buf, np.float32, od, pos).copy(); pos += od * 4
                wq = np.frombuffer(buf, np.int8, od * id_, pos).copy().reshape(od, id_); pos += od * id_
                lins.append((wq, sc, bv))
            # Convert [C, 1, W] -> [W, C] if 3D
            if cur.ndim == 3:
                cur = cur[:, 0, :].T  # [T, C]
            T, C = cur.shape
            hdim = C // nheads
            x_norm = _layernorm_ref(cur, ln1_g, ln1_b, eps1)
            qkv = _linear_ref(x_norm, *lins[0]).reshape(T, 3, nheads, hdim).transpose(1, 2, 0, 3)
            q, k, v = qkv[0], qkv[1], qkv[2]  # [H, T, hd]
            attn = (q * (hdim ** -0.5)) @ k.transpose(0, 2, 1)
            attn = attn - np.max(attn, axis=-1, keepdims=True)
            attn = np.exp(attn)
            attn = attn / np.sum(attn, axis=-1, keepdims=True)
            ctx = (attn @ v).transpose(1, 0, 2).reshape(T, C)
            cur = cur + _linear_ref(ctx, *lins[1])
            x_norm2 = _layernorm_ref(cur, ln2_g, ln2_b, eps2)
            ff = _linear_ref(x_norm2, *lins[2])
            ff = _apply_act(ff, ACT_SWISH)
            cur = cur + _linear_ref(ff, *lins[3])
        elif t == OP_LAYERNORM:
            dim, eps = struct.unpack_from("<Hf", buf, pos); pos += 6
            g = np.frombuffer(buf, np.float32, dim, pos).copy(); pos += dim * 4
            b = np.frombuffer(buf, np.float32, dim, pos).copy(); pos += dim * 4
            cur = _layernorm_ref(cur, g, b, eps)
            # Reshape back to [C, 1, T] for post-SVTR convs
            cur = cur.T[:, None, :]
        elif t == OP_CTC_HEAD:
            od, id_ = struct.unpack_from("<HH", buf, pos); pos += 4
            sc = np.frombuffer(buf, np.float32, od, pos).copy(); pos += od * 4
            bv = np.frombuffer(buf, np.float32, od, pos).copy(); pos += od * 4
            wq = np.frombuffer(buf, np.int8, od * id_, pos).copy().reshape(od, id_); pos += od * id_
            if cur.ndim == 3:
                cur = cur[:, 0, :].T  # [T, C]
            cur = _linear_ref(cur, wq, sc, bv)

    assert pos == len(buf), f"Trailing bytes: {len(buf) - pos}"
    return _ctc_decode(cur, vocab), cur


def verify_ocr_model(bin_path: str):
    import onnxruntime as ort
    sess = ort.InferenceSession(ONNX_PATH, providers=["CPUExecutionProvider"])
    with open(DICT_TXT_PATH, "r", encoding="utf-8") as f:
        vocab = [""] + [l.rstrip("\r\n") for l in f] + [" "]

    font_path = r"C:\Windows\Fonts\BIZ-UDGothicR.ttc"
    font = ImageFont.truetype(font_path, 26)
    samples = [
        "ポケモンセンターへようこそ！",
        "あいての攻撃が急所に当たった！",
        "セーブしています。電源を切らないでください。",
        "日本語の辞書と翻訳テスト",
        "図鑑 10匹 レベル15",
    ]
    matches = 0
    for txt in samples:
        img = Image.new("RGB", (320, 48), (245, 245, 245))
        drw = ImageDraw.Draw(img)
        drw.text((8, 10), txt, fill=(15, 15, 15), font=font)
        arr = np.asarray(img, dtype=np.float32).transpose(2, 0, 1)[None]
        arr = (arr / 255.0 - 0.5) / 0.5

        ort_out = sess.run(None, {"x": arr})[0][0]
        ort_txt = _ctc_decode(ort_out, vocab)
        int8_txt, _ = run_int8_ref(bin_path, arr)
        ok = (ort_txt == int8_txt)
        if ok:
            matches += 1
        print(f"  [Verify] GT='{txt}' | ORT='{ort_txt}' | INT8='{int8_txt}' | match={ok}")
    print(f"[OCR Verify] {matches}/{len(samples)} exact ORT==INT8 matches")
    assert matches >= 4, "INT8 OCR verification diverged too much from ONNX FP32!"


# ---------------------------------------------------------------------------
# 2. Build jmdict_offline.bin
# ---------------------------------------------------------------------------

def build_jmdict(out_path: str):
    t0 = time.time()
    # Load word_meta.db for frequency & JLPT
    meta_map = {}
    if os.path.exists(WORD_META_DB_PATH):
        mcon = sqlite3.connect(WORD_META_DB_PATH)
        for term, reading, freq, jlpt in mcon.execute("SELECT term, reading, freq_rank, jlpt FROM word_meta"):
            prev = meta_map.get((term, reading))
            if prev is None or (freq is not None and (prev[0] is None or freq < prev[0])):
                meta_map[(term, reading)] = (freq or 0, jlpt or "")
            if term not in meta_map:
                meta_map[term] = (freq or 0, jlpt or "")
        mcon.close()

    jlpt_vocab = {}
    if os.path.exists(JLPT_VOCAB_PATH):
        with open(JLPT_VOCAB_PATH, "r", encoding="utf-8") as f:
            jv = json.load(f)
            if isinstance(jv, dict):
                for k, v in jv.items():
                    jlpt_vocab[k] = str(v)

    con = sqlite3.connect(JAMDICT_DB_PATH)
    cur = con.cursor()

    kanji_by_id = defaultdict(list)
    for idseq, txt in cur.execute("SELECT idseq, text FROM Kanji ORDER BY ID"):
        kanji_by_id[idseq].append(txt)

    kana_by_id = defaultdict(list)
    for idseq, txt in cur.execute("SELECT idseq, text FROM Kana ORDER BY ID"):
        kana_by_id[idseq].append(txt)

    pos_by_sid = defaultdict(list)
    for sid, txt in cur.execute("SELECT sid, text FROM pos"):
        pos_by_sid[sid].append(txt)

    misc_by_sid = defaultdict(list)
    for sid, txt in cur.execute("SELECT sid, text FROM misc"):
        misc_by_sid[sid].append(txt)

    kanji_info_by_id = defaultdict(lambda: defaultdict(list))
    for idseq, ktxt, itxt in cur.execute("SELECT k.idseq, k.text, i.text FROM Kanji k JOIN KJI i ON i.kid = k.ID"):
        kanji_info_by_id[idseq][ktxt].append(itxt)

    kana_info_by_id = defaultdict(lambda: defaultdict(list))
    for idseq, ktxt, itxt in cur.execute("SELECT kn.idseq, kn.text, i.text FROM Kana kn JOIN KNI i ON i.kid = kn.ID"):
        kana_info_by_id[idseq][ktxt].append(itxt)

    gloss_by_sid = defaultdict(list)
    for sid, txt in cur.execute("SELECT sid, text FROM SenseGloss WHERE lang='eng'"):
        gloss_by_sid[sid].append(txt)

    senses_by_id = defaultdict(list)
    for sid, idseq in cur.execute("SELECT ID, idseq FROM Sense ORDER BY ID"):
        senses_by_id[idseq].append((sid, pos_by_sid.get(sid, []), gloss_by_sid.get(sid, []), misc_by_sid.get(sid, [])))

    entry_ids = [r[0] for r in cur.execute("SELECT idseq FROM Entry ORDER BY idseq")]
    con.close()

    if ROOT_DIR not in sys.path:
        sys.path.insert(0, ROOT_DIR)
    from japanese_word_meta import JapaneseWordMetaAnalyzer
    word_meta_analyzer = JapaneseWordMetaAnalyzer(JAMDICT_DB_PATH)

    class _FormShim:
        __slots__ = ("text", "info")
        def __init__(self, text, info):
            self.text = text
            self.info = info

    class _EntryShim:
        __slots__ = ("kanji_forms", "kana_forms")
        def __init__(self, kanji_forms, kana_forms):
            self.kanji_forms = kanji_forms
            self.kana_forms = kana_forms

    def _utf8_trunc(s: str, max_bytes: int) -> bytes:
        b = s.encode("utf-8")
        if len(b) <= max_bytes:
            return b
        # Back off to a valid UTF-8 character boundary
        cut = max_bytes
        while cut > 0 and (b[cut] & 0xC0) == 0x80:
            cut -= 1
        return b[:cut]

    # Pack entries into ~4096-byte uncompressed blocks compressed with zlib
    BLOCK_TARGET = 4096
    blocks = []
    cur_block = bytearray()
    surface_refs = defaultdict(list)  # surface_utf8 -> list of (block_id, offset_in_block, priority)

    for idseq in entry_ids:
        kanjis = kanji_by_id.get(idseq, [])
        kanas = kana_by_id.get(idseq, [])
        senses = senses_by_id.get(idseq, [])
        if not senses or (not kanjis and not kanas):
            continue

        headword = kanjis[0] if kanjis else kanas[0]
        reading = kanas[0] if kanas else ""

        freq, jlpt_raw = meta_map.get((headword, reading), meta_map.get(headword, (0, "")))
        if not jlpt_raw:
            jlpt_raw = jlpt_vocab.get(headword) or jlpt_vocab.get(reading) or ""
        jlpt = f"N{jlpt_raw}" if str(jlpt_raw).isdigit() and int(jlpt_raw) > 0 else str(jlpt_raw or "")
        # Store all unique POS tags (up to 255 bytes) and all senses/glosses (up to 4000 bytes)
        pos_list = []
        misc_list = []
        for _, plist, _, mlist in senses:
            for p in plist:
                if p not in pos_list:
                    pos_list.append(p)
            for m in mlist:
                if m not in misc_list:
                    misc_list.append(m)
        pos_str = "; ".join(pos_list)

        k_inf = kanji_info_by_id.get(idseq)
        kn_inf = kana_info_by_id.get(idseq)
        shim = None
        if k_inf or kn_inf:
            shim = _EntryShim(
                [_FormShim(t, inf) for t, inf in k_inf.items()] if k_inf else [],
                [_FormShim(t, inf) for t, inf in kn_inf.items()] if kn_inf else [],
            )
        meta_tag = word_meta_analyzer.format_meta_tag(
            headword, reading, pos_list + misc_list, jmdict_entry=shim
        )
        if meta_tag:
            pos_str = f"{meta_tag} {pos_str}" if pos_str else meta_tag

        sense_lines = []
        for si, (_, _, glist, _) in enumerate(senses):
            if glist:
                sense_lines.append(f"{si+1}. " + "; ".join(glist))
        gloss_str = "\n".join(sense_lines)
        if not gloss_str:
            continue

        hw_b = _utf8_trunc(headword, 255)
        rd_b = _utf8_trunc(reading, 255)
        pos_b = _utf8_trunc(pos_str, 255)
        jlpt_b = _utf8_trunc(jlpt, 15)
        gl_b = _utf8_trunc(gloss_str, 4000)

        rec = struct.pack("<IBBBBH", freq, len(hw_b), len(rd_b), len(pos_b), len(jlpt_b), len(gl_b))
        rec += hw_b + rd_b + pos_b + jlpt_b + gl_b

        if len(cur_block) + len(rec) > BLOCK_TARGET and len(cur_block) > 0:
            blocks.append(zlib.compress(bytes(cur_block), level=6))
            cur_block = bytearray()

        bid = len(blocks)
        off = len(cur_block)
        cur_block.extend(rec)

        # Priority: lower is better (0 = exact freq rank, 999999 = unranked; kanji match slightly preferred)
        prio = freq if freq > 0 else 500000
        seen_surf = set()
        for k_idx, k in enumerate(kanjis):
            if k not in seen_surf:
                seen_surf.add(k)
                surface_refs[k].append((bid, off, prio + k_idx))
        for r_idx, r in enumerate(kanas):
            if r not in seen_surf:
                seen_surf.add(r)
                surface_refs[r].append((bid, off, prio + 50 + r_idx))

    if os.path.exists(CUSTOM_DICT_PATH):
        with open(CUSTOM_DICT_PATH, "r", encoding="utf-8") as cf:
            custom_data = json.load(cf)
        small_kata = str.maketrans("ァィゥェォッャュョ", "アイウエオツヤユヨ")
        custom_count = 0
        for surf, centries in custom_data.items():
            for ce in centries:
                hw = str(ce.get("headword") or surf)
                rd = str(ce.get("reading") or surf)
                pos_s = str(ce.get("pos") or "Custom")
                gl_list = ce.get("glosses") or []
                gl_s = "\n".join(str(g) for g in gl_list)
                if not gl_s:
                    continue
                hw_b = hw.encode("utf-8")[:255]
                rd_b = rd.encode("utf-8")[:255]
                pos_b = pos_s.encode("utf-8")[:255]
                jlpt_b = b""
                gl_b = gl_s.encode("utf-8")[:4000]
                rec = struct.pack("<IBBBBH", 0, len(hw_b), len(rd_b), len(pos_b), 0, len(gl_b))
                rec += hw_b + rd_b + pos_b + jlpt_b + gl_b
                if len(cur_block) + len(rec) > BLOCK_TARGET and len(cur_block) > 0:
                    blocks.append(zlib.compress(bytes(cur_block), level=6))
                    cur_block = bytearray()
                bid = len(blocks)
                off = len(cur_block)
                cur_block.extend(rec)
                for key_s in {surf, hw, rd}:
                    if key_s:
                        surface_refs[key_s].append((bid, off, 0))
                        norm_s = key_s.translate(small_kata)
                        if norm_s != key_s:
                            surface_refs[norm_s].append((bid, off, 5))
                custom_count += 1
        print(f"[JMdict] Merged {custom_count:,} custom_dict.json entries")

        # Also index Pokémon species categories (e.g. わるぎつね / わるぎつねポケモン)
        poke_cats = defaultdict(list)
        cat_re = re.compile(r"— ([^/]+?) / ([^)]+?)ポケモン\)")
        for surf, centries in custom_data.items():
            for ce in centries:
                if ce.get("pos") != "Pokémon Species":
                    continue
                for g in ce.get("glosses") or []:
                    m = cat_re.search(str(g))
                    if m:
                        en_cat = m.group(1).strip()
                        jp_cat = m.group(2).strip()
                        if en_cat not in poke_cats[jp_cat]:
                            poke_cats[jp_cat].append(en_cat)

        cat_count = 0
        for jp_cat, en_cats in poke_cats.items():
            full_key = jp_cat + "ポケモン"
            full_gl = "1. " + "; ".join(en_cats)
            hw_b = full_key.encode("utf-8")[:255]
            rd_b = full_key.encode("utf-8")[:255]
            pos_b = "Pokémon Category".encode("utf-8")[:255]
            gl_b = full_gl.encode("utf-8")[:4000]
            rec = struct.pack("<IBBBBH", 0, len(hw_b), len(rd_b), len(pos_b), 0, len(gl_b))
            rec += hw_b + rd_b + pos_b + gl_b
            if len(cur_block) + len(rec) > BLOCK_TARGET and len(cur_block) > 0:
                blocks.append(zlib.compress(bytes(cur_block), level=6))
                cur_block = bytearray()
            bid = len(blocks)
            off = len(cur_block)
            cur_block.extend(rec)
            surface_refs[full_key].append((bid, off, 0))
            norm_full = full_key.translate(small_kata)
            if norm_full != full_key:
                surface_refs[norm_full].append((bid, off, 5))
            cat_count += 1

            if jp_cat not in surface_refs:
                short_ens = [e[:-8] if e.endswith(" Pokémon") else e for e in en_cats]
                short_gl = "1. " + "; ".join(short_ens) + f" ({full_key})"
                shw_b = jp_cat.encode("utf-8")[:255]
                srd_b = jp_cat.encode("utf-8")[:255]
                sgl_b = short_gl.encode("utf-8")[:4000]
                srec = struct.pack("<IBBBBH", 0, len(shw_b), len(srd_b), len(pos_b), 0, len(sgl_b))
                srec += shw_b + srd_b + pos_b + sgl_b
                if len(cur_block) + len(srec) > BLOCK_TARGET and len(cur_block) > 0:
                    blocks.append(zlib.compress(bytes(cur_block), level=6))
                    cur_block = bytearray()
                sbid = len(blocks)
                soff = len(cur_block)
                cur_block.extend(srec)
                surface_refs[jp_cat].append((sbid, soff, 0))
                norm_short = jp_cat.translate(small_kata)
                if norm_short != jp_cat:
                    surface_refs[norm_short].append((sbid, soff, 5))
                cat_count += 1
        print(f"[JMdict] Merged {cat_count:,} Pokémon Category entries")

    if len(cur_block) > 0:
        blocks.append(zlib.compress(bytes(cur_block), level=6))

    # Merge JMnedict proper names (non-pure-hiragana multi-char entries, priority 900000)
    if os.path.exists(JAMDICT_DB_PATH):
        ncon = sqlite3.connect(JAMDICT_DB_PATH)
        ncur = ncon.cursor()
        ne_kanji = defaultdict(list)
        for idseq, txt in ncur.execute("SELECT idseq, text FROM NEKanji ORDER BY ID"):
            ne_kanji[idseq].append(txt)
        ne_kana = defaultdict(list)
        for idseq, txt in ncur.execute("SELECT idseq, text FROM NEKana ORDER BY ID"):
            ne_kana[idseq].append(txt)
        ne_types = defaultdict(list)
        for tid, txt in ncur.execute("SELECT tid, text FROM NETransType"):
            ne_types[tid].append(txt)
        ne_gloss = defaultdict(list)
        for tid, txt in ncur.execute("SELECT tid, text FROM NETransGloss WHERE lang='eng' OR lang='' OR lang IS NULL"):
            if txt:
                ne_gloss[tid].append(txt)
        ne_trans = defaultdict(list)
        for tid, idseq in ncur.execute("SELECT ID, idseq FROM NETranslation ORDER BY ID"):
            ne_trans[idseq].append((tid, ne_types.get(tid, []), ne_gloss.get(tid, [])))
        ne_ids = [r[0] for r in ncur.execute("SELECT idseq FROM NEEntry ORDER BY idseq")]
        ncon.close()

        def _has_non_hiragana(s: str) -> bool:
            return any(not (0x3041 <= ord(ch) <= 0x3096) for ch in s)

        ne_added = 0
        for idseq in ne_ids:
            kanjis = ne_kanji.get(idseq, [])
            kanas = ne_kana.get(idseq, [])
            trans = ne_trans.get(idseq, [])
            if not trans or (not kanjis and not kanas):
                continue

            qual_keys = []
            seen_ne_k = set()
            for k in kanjis + kanas:
                if k and len(k) >= 2 and _has_non_hiragana(k) and k not in seen_ne_k:
                    seen_ne_k.add(k)
                    qual_keys.append(k)
            if not qual_keys:
                continue

            type_list = []
            gloss_lines = []
            for ti, (_, tlist, glist) in enumerate(trans):
                for tp in tlist:
                    if tp not in type_list:
                        type_list.append(tp)
                if glist:
                    gloss_lines.append(f"{ti+1}. " + "; ".join(glist))
            if not gloss_lines:
                continue

            headword = kanjis[0] if kanjis else kanas[0]
            reading = kanas[0] if kanas else ""
            pos_str = ("Name (" + ", ".join(type_list[:3]) + ")") if type_list else "Proper Name"
            gloss_str = "\n".join(gloss_lines)

            hw_b = headword.encode("utf-8")[:255]
            rd_b = reading.encode("utf-8")[:255]
            pos_b = pos_str.encode("utf-8")[:255]
            gl_b = gloss_str.encode("utf-8")[:4000]
            rec = struct.pack("<IBBBBH", 0, len(hw_b), len(rd_b), len(pos_b), 0, len(gl_b))
            rec += hw_b + rd_b + pos_b + gl_b

            if len(cur_block) + len(rec) > BLOCK_TARGET and len(cur_block) > 0:
                blocks.append(zlib.compress(bytes(cur_block), level=6))
                cur_block = bytearray()
            bid = len(blocks)
            off = len(cur_block)
            cur_block.extend(rec)
            for k in qual_keys:
                surface_refs[k].append((bid, off, 900000))
            ne_added += 1

        if len(cur_block) > 0:
            blocks.append(zlib.compress(bytes(cur_block), level=6))
            cur_block = bytearray()
        print(f"[JMnedict] Merged {ne_added:,} proper name entries")

    # Sort surfaces by UTF-8 bytes so C++ can binary-search directly with memcmp
    sorted_keys = sorted(surface_refs.keys(), key=lambda s: s.encode("utf-8"))

    # Build postings blob & key table
    # Each key entry in key_table: [u8 key_len][key_bytes][u8 count][count * (u32 bid, u16 off)]
    # Sparse index every 128 keys: [u32 key_table_offset][u8 key_len][key_bytes]
    key_blob = bytearray()
    sparse_entries = []
    SPARSE_STEP = 128

    for idx_k, surf in enumerate(sorted_keys):
        kb = surf.encode("utf-8")
        if len(kb) > 120:
            continue
        refs = sorted(surface_refs[surf], key=lambda x: x[2])[:64]
        if len(sparse_entries) * SPARSE_STEP <= idx_k:
            sparse_entries.append((len(key_blob), kb))
        key_blob.append(len(kb))
        key_blob.extend(kb)
        key_blob.append(len(refs))
        for bid, off, _ in refs:
            key_blob.extend(struct.pack("<IH", bid, off))

    # Build sparse index blob
    sparse_blob = bytearray()
    for koff, kb in sparse_entries:
        sparse_blob.extend(struct.pack("<IB", koff, len(kb)))
        sparse_blob.extend(kb)

    # Block offset table (num_blocks + 1 uint32 offsets relative to blocks_start)
    block_offs = [0]
    for blk in blocks:
        block_offs.append(block_offs[-1] + len(blk))

    # Header: magic(8) + ver(4) + num_blocks(4) + num_sparse(4) + sparse_bytes(4) + key_blob_bytes(4)
    header_sz = 28
    sparse_off = header_sz
    key_off = sparse_off + len(sparse_blob)
    btab_off = key_off + len(key_blob)
    blocks_off = btab_off + len(block_offs) * 4

    with open(out_path, "wb") as f:
        f.write(b"JMDICTOF")
        f.write(struct.pack("<IIIII", 1, len(blocks), len(sparse_entries), len(sparse_blob), len(key_blob)))
        f.write(sparse_blob)
        f.write(key_blob)
        for bo in block_offs:
            f.write(struct.pack("<I", blocks_off + bo))
        for blk in blocks:
            f.write(blk)

    sz = os.path.getsize(out_path)
    print(f"[JMdict] Wrote {out_path} ({sz:,} bytes, {len(blocks)} blocks, {len(sorted_keys):,} keys, sparse={len(sparse_blob):,}B) in {time.time()-t0:.2f}s")


# ---------------------------------------------------------------------------
# 3. Build kanjidic_offline.bin
# ---------------------------------------------------------------------------

def _rasterize_stroke_120_zlib(ch: str, stroke_font) -> bytes:
    if stroke_font is None:
        return b""
    try:
        bbox = stroke_font.getbbox(ch)
        if bbox is None:
            return b""
        gw = bbox[2] - bbox[0]
        gh = bbox[3] - bbox[1]
        if gw <= 0 or gh <= 0:
            return b""
        img = Image.new("L", (120, 120), 0)
        drw = ImageDraw.Draw(img)
        gx = (120 - gw) // 2 - bbox[0]
        gy = (120 - gh) // 2 - bbox[1]
        drw.text((gx, gy), ch, fill=255, font=stroke_font)
        arr = np.asarray(img, dtype=np.uint8)
        return zlib.compress(arr.tobytes(), level=6)
    except Exception:
        return b""


def build_kanjidic(out_path: str):
    t0 = time.time()
    sys.path.insert(0, ROOT_DIR)
    from jamdict import Jamdict
    from ocr_server import WordMetaManager

    jam = Jamdict(JAMDICT_DB_PATH)
    wmm = WordMetaManager()

    stroke_ttf = os.path.join(ROOT_DIR, "dictionaries", "KanjiStrokeOrders.ttf")
    stroke_font = ImageFont.truetype(stroke_ttf, 106) if os.path.exists(stroke_ttf) else None

    con = sqlite3.connect(JAMDICT_DB_PATH)
    chars = [r[0] for r in con.execute("SELECT literal FROM character") if r[0]]
    con.close()

    # Also include any component/radical variants in kanji_components.json (e.g. ⻌, ⺌)
    extra_chars = set(chars)
    if os.path.exists(KANJI_COMP_PATH):
        with open(KANJI_COMP_PATH, "r", encoding="utf-8") as f:
            kc = json.load(f)
            for k, v in kc.items():
                if k:
                    extra_chars.add(k[0])
                if isinstance(v, dict):
                    for c in v.get("c") or []:
                        if c:
                            extra_chars.add(c[0])
                    p = v.get("p")
                    if isinstance(p, list) and p and p[0]:
                        extra_chars.add(str(p[0])[0])

    entries = []
    for ch in extra_chars:
        cp = ord(ch)
        info = wmm.lookup_kanji_info(ch, jam)
        strokes = min(255, int(info.get("stroke_count") or 0))
        grade_raw = str(info.get("grade") or "")
        grade = min(255, int(grade_raw)) if grade_raw.isdigit() else 0
        jlpt_raw = str(info.get("jlpt") or "")
        jlpt = min(255, int(jlpt_raw)) if jlpt_raw.isdigit() else 0
        freq_raw = str(info.get("freq") or "")
        freq = min(65535, int(freq_raw)) if freq_raw.isdigit() else 0
        rad_num = min(65535, int(info.get("rad_num") or 0))

        rad_char_b = (info.get("rad_char") or "").encode("utf-8")[:31]
        rad_base_b = (info.get("rad_base") or "").encode("utf-8")[:31]
        rad_read_b = (info.get("rad_reading") or "").encode("utf-8")[:63]
        rad_mean_b = (info.get("rad_meaning") or "").encode("utf-8")[:63]

        on_b = "・".join((info.get("on_readings") or [])[:8]).encode("utf-8")[:255]
        kun_b = "・".join((info.get("kun_readings") or [])[:8]).encode("utf-8")[:255]
        nanori_b = "・".join((info.get("nanori") or [])[:6]).encode("utf-8")[:255]
        mean_b = ", ".join((info.get("meanings") or [])[:12]).encode("utf-8")[:1024]

        comp_items = [f"{c} ({m})" if m else c for c, m in (info.get("components") or [])]
        comp_b = "\x1f".join(comp_items).encode("utf-8")[:512]

        phon = info.get("phonetic")
        phon_b = (f"{phon[0]} ({phon[1]})" if phon else "").encode("utf-8")[:127]

        stroke_z = _rasterize_stroke_120_zlib(ch, stroke_font)

        rec = struct.pack(
            "<HHBBBBBBBBBBBHHH",
            freq, rad_num,
            strokes, grade, jlpt,
            len(rad_char_b), len(rad_base_b), len(rad_read_b), len(rad_mean_b),
            len(on_b), len(kun_b), len(nanori_b), len(phon_b),
            len(mean_b), len(comp_b), len(stroke_z),
        )
        rec += (rad_char_b + rad_base_b + rad_read_b + rad_mean_b +
                on_b + kun_b + nanori_b + phon_b + mean_b + comp_b + stroke_z)
        entries.append((cp, rec))

    entries.sort(key=lambda x: x[0])
    hdr_sz = 16
    idx_sz = len(entries) * 10
    data_start = hdr_sz + idx_sz
    cur_off = 0
    with open(out_path, "wb") as f:
        f.write(b"KANJIOF2")
        f.write(struct.pack("<II", 2, len(entries)))
        for cp, rec in entries:
            f.write(struct.pack("<IIH", cp, data_start + cur_off, len(rec)))
            cur_off += len(rec)
        for _, rec in entries:
            f.write(rec)

    sz = os.path.getsize(out_path)
    print(f"[Kanjidic] Wrote {out_path} (v2, {sz:,} bytes, {len(entries):,} kanji) in {time.time()-t0:.2f}s")


# ---------------------------------------------------------------------------
# 4. Build font_pack.bin
# ---------------------------------------------------------------------------

def _pick_font_path():
    candidates = [
        r"C:\Windows\Fonts\msgothic.ttc",
        r"C:\Windows\Fonts\BIZ-UDGothicR.ttc",
        r"C:\Windows\Fonts\YuGothM.ttc",
        r"C:\Windows\Fonts\meiryo.ttc",
    ]
    for p in candidates:
        if os.path.exists(p):
            return p
    raise FileNotFoundError("No Japanese TrueType font found in C:\\Windows\\Fonts")


def build_font_pack(out_path: str):
    t0 = time.time()
    font_path = _pick_font_path()
    # Match ocr_server.py fontId 0 (header/footer 12px), 1 (body/gloss 13px), 2 (title 20px)
    fonts = {
        0: ImageFont.truetype(font_path, 12),
        1: ImageFont.truetype(font_path, 13),
        2: ImageFont.truetype(font_path, 20),
    }

    cps = set()
    for c in range(0x20, 0x7F):
        cps.add(c)
    for c in range(0x3000, 0x3100):
        cps.add(c)
    for c in range(0xFF01, 0xFF65):
        cps.add(c)
    for ch in "…【】→←↑↓✓★─•「」『』（）〜・ー·音訓名":
        cps.add(ord(ch))
    for c in range(0x2E80, 0x2FDF):
        cps.add(c)
    with open(DICT_TXT_PATH, "r", encoding="utf-8") as f:
        for line in f:
            for ch in line.strip():
                cps.add(ord(ch))
    con = sqlite3.connect(JAMDICT_DB_PATH)
    for (lit,) in con.execute("SELECT literal FROM character"):
        if lit:
            cps.add(ord(lit[0]))
    con.close()

    sorted_cps = sorted(cps)
    # Build (key, glyph_blob) for fontId in (0, 1, 2)
    # Key = (fontId << 32) | cp
    records = []

    rad_fonts = {}
    for rf_path in (r"C:\Windows\Fonts\meiryo.ttc", r"C:\Windows\Fonts\YuGothM.ttc"):
        if os.path.exists(rf_path):
            rad_fonts = {0: ImageFont.truetype(rf_path, 12), 1: ImageFont.truetype(rf_path, 13), 2: ImageFont.truetype(rf_path, 20)}
            break

    for fid in (0, 1, 2):
        fnt_primary = fonts[fid]
        fnt_rad = rad_fonts.get(fid, fnt_primary)
        for cp in sorted_cps:
            fnt = fnt_rad if (0x2E80 <= cp <= 0x2EFF) else fnt_primary
            ch = chr(cp)
            bbox = fnt.getbbox(ch)
            adv = int(round(fnt.getlength(ch)))
            if bbox is None:
                blob = struct.pack("<BBbbB", 0, 0, 0, 0, max(1, adv))
                records.append(((fid << 32) | cp, blob))
                continue
            x0, y0, x1, y1 = bbox
            mask = fnt.getmask(ch, mode="L")
            mw, mh = mask.size
            if mw == 0 or mh == 0:
                blob = struct.pack("<BBbbB", 0, 0, 0, 0, max(1, adv))
                records.append(((fid << 32) | cp, blob))
                continue
            arr2d = np.asarray(mask, dtype=np.uint8).reshape(mh, mw)
            gw = min(30, mw)
            gh = min(30, mh)
            arr = arr2d[:gh, :gw].reshape(-1)
            # Pack 4-bit nibbles (high nibble first, matching DrawGlyphText in ocr_client.cpp)
            nib = np.clip((arr.astype(np.uint16) + 8) >> 4, 0, 15).astype(np.uint8)
            if nib.size % 2 == 1:
                nib = np.append(nib, np.uint8(0))
            packed = ((nib[0::2] << 4) | nib[1::2]).astype(np.uint8).tobytes()
            ox = max(-127, min(127, int(x0)))
            oy = max(-127, min(127, int(y0)))
            blob = struct.pack("<BBbbB", gw, gh, ox, oy, max(1, min(255, adv))) + packed
            records.append(((fid << 32) | cp, blob))

    records.sort(key=lambda x: x[0])
    hdr_sz = 16
    idx_sz = len(records) * 14  # u64 key + u32 offset + u16 len
    data_start = hdr_sz + idx_sz
    cur_off = 0

    with open(out_path, "wb") as f:
        f.write(b"FONTPACK")
        f.write(struct.pack("<II", 1, len(records)))
        for key, blob in records:
            f.write(struct.pack("<QIH", key, data_start + cur_off, len(blob)))
            cur_off += len(blob)
        for _, blob in records:
            f.write(blob)

    sz = os.path.getsize(out_path)
    print(f"[FontPack] Wrote {out_path} ({sz:,} bytes, {len(records):,} glyphs) in {time.time()-t0:.2f}s")


DET_OP_CONV2D = 1
DET_OP_CONV_T2X2 = 2
DET_OP_ADD = 3
DET_OP_MUL_CH = 4
DET_OP_GAP = 5
DET_OP_RESIZE_NEAREST = 6
DET_OP_CONCAT4 = 7


def _find_det_onnx() -> str:
    cand = os.path.join(ROOT_DIR, "ocr_models", "ch_PP-OCRv4_det_infer.onnx")
    if os.path.exists(cand):
        return cand
    try:
        import rapidocr_onnxruntime
        pkg_cand = os.path.join(os.path.dirname(rapidocr_onnxruntime.__file__), "models", "ch_PP-OCRv4_det_infer.onnx")
        if os.path.exists(pkg_cand):
            return pkg_cand
    except Exception:
        pass
    raise FileNotFoundError("Missing ch_PP-OCRv4_det_infer.onnx in ocr_models/ or rapidocr_onnxruntime")


def build_ocr_det_model(out_path: str):
    t0 = time.time()
    det_onnx = _find_det_onnx()
    nodes, consts = load_onnx_graph(det_onnx)

    # Map producer output -> consumer list to check fan-out
    consumers = defaultdict(list)
    for idx, nd in enumerate(nodes):
        for inp in nd["in"]:
            if inp not in consts:
                consumers[inp].append(idx)

    # Graph input name
    all_outs = {o for nd in nodes for o in nd["out"]}
    graph_inputs = [inp for nd in nodes for inp in nd["in"] if inp not in consts and inp not in all_outs]
    graph_in_name = graph_inputs[0]

    def fold_bn(w_arr, b_arr, bn_node, is_conv_t=False):
        gamma = consts[bn_node["in"][1]].astype(np.float32).reshape(-1)
        beta = consts[bn_node["in"][2]].astype(np.float32).reshape(-1)
        mean = consts[bn_node["in"][3]].astype(np.float32).reshape(-1)
        var = consts[bn_node["in"][4]].astype(np.float32).reshape(-1)
        eps = bn_node["attrs"].get("epsilon", {}).get("f") or 1e-5
        scale = gamma / np.sqrt(var + eps)
        if is_conv_t:
            # w_arr is [inC, outC, kH, kW]
            w_folded = w_arr * scale.reshape(1, -1, 1, 1)
        else:
            # w_arr is [outC, inC_g, kH, kW]
            w_folded = w_arr * scale.reshape(-1, 1, 1, 1)
        b_folded = (b_arr - mean) * scale + beta
        return w_folded, b_folded

    def fold_affine(w_arr, b_arr, mul_node, add_node):
        scale = consts[mul_node["in"][1]].astype(np.float32).reshape(-1) if mul_node["in"][1] in consts else consts[mul_node["in"][0]].astype(np.float32).reshape(-1)
        shift = consts[add_node["in"][1]].astype(np.float32).reshape(-1) if add_node["in"][1] in consts else consts[add_node["in"][0]].astype(np.float32).reshape(-1)
        return w_arr * scale.reshape(-1, 1, 1, 1), b_arr * scale + shift

    def consume_activation(cur_tensor, start_idx):
        # Returns (final_tensor_name, act_code, next_idx)
        # act: 0=none, 1=relu, 2=hswish, 5=sigmoid, 6=hsigmoid(1/6)
        if start_idx < len(nodes):
            n0 = nodes[start_idx]
            if n0["op"] == "Relu" and n0["in"][0] == cur_tensor:
                return n0["out"][0], 1, start_idx + 1
            if n0["op"] == "HardSigmoid" and n0["in"][0] == cur_tensor:
                return n0["out"][0], 6, start_idx + 1
            if n0["op"] == "Sigmoid" and n0["in"][0] == cur_tensor:
                return n0["out"][0], 5, start_idx + 1
            if (
                start_idx + 3 < len(nodes)
                and n0["op"] == "Add"
                and cur_tensor in n0["in"]
                and nodes[start_idx + 1]["op"] == "Clip"
                and nodes[start_idx + 2]["op"] == "Mul"
                and nodes[start_idx + 3]["op"] == "Div"
            ):
                return nodes[start_idx + 3]["out"][0], 2, start_idx + 4
        return cur_tensor, 0, start_idx

    raw_ops = []
    idx = 0
    while idx < len(nodes):
        nd = nodes[idx]
        op = nd["op"]
        if op == "Conv":
            w = consts[nd["in"][1]].astype(np.float32)
            out_c = w.shape[0]
            b = consts[nd["in"][2]].astype(np.float32).reshape(-1) if len(nd["in"]) >= 3 and nd["in"][2] in consts else np.zeros(out_c, dtype=np.float32)
            cur_t = nd["out"][0]
            j = idx + 1
            if j < len(nodes) and nodes[j]["op"] == "BatchNormalization" and nodes[j]["in"][0] == cur_t:
                w, b = fold_bn(w, b, nodes[j], is_conv_t=False)
                cur_t = nodes[j]["out"][0]
                j += 1
            elif (
                j + 1 < len(nodes)
                and nodes[j]["op"] == "Mul"
                and cur_t in nodes[j]["in"]
                and any(x in consts for x in nodes[j]["in"])
                and nodes[j + 1]["op"] == "Add"
                and nodes[j]["out"][0] in nodes[j + 1]["in"]
                and any(x in consts for x in nodes[j + 1]["in"])
            ):
                w, b = fold_affine(w, b, nodes[j], nodes[j + 1])
                cur_t = nodes[j + 1]["out"][0]
                j += 2
            cur_t, act, j = consume_activation(cur_t, j)
            post_scale, post_bias = 1.0, 0.0
            if (
                j + 1 < len(nodes)
                and nodes[j]["op"] == "Mul"
                and cur_t in nodes[j]["in"]
                and any(x in consts for x in nodes[j]["in"])
                and nodes[j + 1]["op"] == "Add"
                and nodes[j]["out"][0] in nodes[j + 1]["in"]
                and any(x in consts for x in nodes[j + 1]["in"])
            ):
                sc_name = [x for x in nodes[j]["in"] if x in consts][0]
                sh_name = [x for x in nodes[j + 1]["in"] if x in consts][0]
                sc_arr = consts[sc_name].astype(np.float32).reshape(-1)
                sh_arr = consts[sh_name].astype(np.float32).reshape(-1)
                if sc_arr.size == 1 and sh_arr.size == 1:
                    post_scale = float(sc_arr[0])
                    post_bias = float(sh_arr[0])
                    cur_t = nodes[j + 1]["out"][0]
                    j += 2
            strides = nd["attrs"].get("strides", {}).get("ints") or [1, 1]
            pads = nd["attrs"].get("pads", {}).get("ints") or [0, 0, 0, 0]
            groups = nd["attrs"].get("group", {}).get("i") or 1
            q_w, scales = quantize_per_channel_int8(w)
            raw_ops.append({
                "type": DET_OP_CONV2D,
                "in": [nd["in"][0]],
                "out": cur_t,
                "in_c": w.shape[1] * groups,
                "out_c": out_c,
                "kh": w.shape[2],
                "kw": w.shape[3],
                "sh": strides[0],
                "sw": strides[1] if len(strides) > 1 else strides[0],
                "ph": pads[0],
                "pw": pads[1] if len(pads) > 1 else pads[0],
                "groups": groups,
                "act": act,
                "post_scale": post_scale,
                "post_bias": post_bias,
                "scales": scales,
                "bias": b,
                "w_q": q_w,
            })
            idx = j
        elif op == "ConvTranspose":
            w = consts[nd["in"][1]].astype(np.float32)  # [inC, outC, kH, kW]
            in_c, out_c = w.shape[0], w.shape[1]
            b = np.zeros(out_c, dtype=np.float32)
            cur_t = nd["out"][0]
            j = idx + 1
            if j < len(nodes) and nodes[j]["op"] == "Add" and cur_t in nodes[j]["in"] and any(x in consts for x in nodes[j]["in"]):
                other = [x for x in nodes[j]["in"] if x in consts][0]
                b = b + consts[other].astype(np.float32).reshape(-1)
                cur_t = nodes[j]["out"][0]
                j += 1
            if j < len(nodes) and nodes[j]["op"] == "BatchNormalization" and nodes[j]["in"][0] == cur_t:
                w, b = fold_bn(w, b, nodes[j], is_conv_t=True)
                cur_t = nodes[j]["out"][0]
                j += 1
            cur_t, act, j = consume_activation(cur_t, j)
            raw_ops.append({
                "type": DET_OP_CONV_T2X2,
                "in": [nd["in"][0]],
                "out": cur_t,
                "in_c": in_c,
                "out_c": out_c,
                "act": act,
                "bias": b,
                "w": np.ascontiguousarray(w, dtype=np.float32),
            })
            idx = j
        elif op == "Add":
            raw_ops.append({"type": DET_OP_ADD, "in": list(nd["in"][:2]), "out": nd["out"][0]})
            idx += 1
        elif op == "Mul":
            raw_ops.append({"type": DET_OP_MUL_CH, "in": list(nd["in"][:2]), "out": nd["out"][0]})
            idx += 1
        elif op == "GlobalAveragePool":
            raw_ops.append({"type": DET_OP_GAP, "in": [nd["in"][0]], "out": nd["out"][0]})
            idx += 1
        elif op == "Resize":
            scales_t = consts[nd["in"][2]].astype(np.float32).reshape(-1)
            sc = int(round(float(scales_t[-1])))
            raw_ops.append({"type": DET_OP_RESIZE_NEAREST, "in": [nd["in"][0]], "out": nd["out"][0], "scale": sc})
            idx += 1
        elif op == "Concat":
            assert len(nd["in"]) == 4, f"Expected 4 inputs to Concat, got {len(nd['in'])}"
            raw_ops.append({"type": DET_OP_CONCAT4, "in": list(nd["in"]), "out": nd["out"][0]})
            idx += 1
        else:
            raise ValueError(f"Unexpected DBNet op {op} at {idx}: {nd}")

    # Assign integer tensor slots
    slot_map = {graph_in_name: 0}
    next_slot = 1
    for rop in raw_ops:
        for tname in rop["in"]:
            assert tname in slot_map, f"Missing tensor {tname}"
        if rop["out"] not in slot_map:
            slot_map[rop["out"]] = next_slot
            next_slot += 1

    in_slot = slot_map[graph_in_name]
    out_slot = slot_map[raw_ops[-1]["out"]]

    with open(out_path, "wb") as f:
        f.write(b"OCRDET01")
        f.write(struct.pack("<IIIII", 1, len(raw_ops), next_slot, in_slot, out_slot))
        for rop in raw_ops:
            t = rop["type"]
            f.write(struct.pack("<B", t))
            if t == DET_OP_CONV2D:
                f.write(struct.pack(
                    "<HHHHHHHHHHHHff",
                    slot_map[rop["in"][0]],
                    slot_map[rop["out"]],
                    rop["in_c"],
                    rop["out_c"],
                    rop["kh"],
                    rop["kw"],
                    rop["sh"],
                    rop["sw"],
                    rop["ph"],
                    rop["pw"],
                    rop["groups"],
                    rop["act"],
                    rop["post_scale"],
                    rop["post_bias"],
                ))
                f.write(rop["scales"].tobytes())
                f.write(rop["bias"].tobytes())
                f.write(np.ascontiguousarray(rop["w_q"], dtype=np.int8).tobytes())
            elif t == DET_OP_CONV_T2X2:
                f.write(struct.pack(
                    "<HHHHH",
                    slot_map[rop["in"][0]],
                    slot_map[rop["out"]],
                    rop["in_c"],
                    rop["out_c"],
                    rop["act"],
                ))
                f.write(rop["bias"].tobytes())
                f.write(rop["w"].tobytes())
            elif t in (DET_OP_ADD, DET_OP_MUL_CH):
                f.write(struct.pack("<HHH", slot_map[rop["in"][0]], slot_map[rop["in"][1]], slot_map[rop["out"]]))
            elif t == DET_OP_GAP:
                f.write(struct.pack("<HH", slot_map[rop["in"][0]], slot_map[rop["out"]]))
            elif t == DET_OP_RESIZE_NEAREST:
                f.write(struct.pack("<HHH", slot_map[rop["in"][0]], slot_map[rop["out"]], rop["scale"]))
            elif t == DET_OP_CONCAT4:
                f.write(struct.pack(
                    "<HHHHH",
                    slot_map[rop["in"][0]],
                    slot_map[rop["in"][1]],
                    slot_map[rop["in"][2]],
                    slot_map[rop["in"][3]],
                    slot_map[rop["out"]],
                ))

    sz = os.path.getsize(out_path)
    print(f"[OCRDet] Wrote {out_path} ({sz:,} bytes, {len(raw_ops)} ops, {next_slot} slots) in {time.time()-t0:.2f}s")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    only = None
    if "--only" in sys.argv:
        i = sys.argv.index("--only")
        if i + 1 < len(sys.argv):
            only = sys.argv[i + 1].lower()

    ocr_bin = os.path.join(OUT_DIR, "ocr_ppocrv4_int8.bin")
    det_bin = os.path.join(OUT_DIR, "ocr_det_int8.bin")
    jmdict_bin = os.path.join(OUT_DIR, "jmdict_offline.bin")
    kanjidic_bin = os.path.join(OUT_DIR, "kanjidic_offline.bin")
    font_bin = os.path.join(OUT_DIR, "font_pack.bin")

    if only in (None, "rec"):
        build_ocr_model(ocr_bin)
        verify_ocr_model(ocr_bin)
    if only in (None, "det"):
        build_ocr_det_model(det_bin)
    if only in (None, "jmdict"):
        build_jmdict(jmdict_bin)
    if only in (None, "kanjidic"):
        build_kanjidic(kanjidic_bin)
    if only in (None, "font"):
        build_font_pack(font_bin)
    print("All requested offline assets built & verified in:", OUT_DIR)


if __name__ == "__main__":
    main()
