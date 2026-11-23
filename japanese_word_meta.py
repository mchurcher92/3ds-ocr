"""
japanese_word_meta.py - Reading type (音読み/訓読み) and honorific prefix (お〜/ご〜) analyzer.

Uses KANJIDIC2 kanji readings and JMdict attested honorific entries from jamdict.db,
combined with curated linguistic exception tables, to annotate Japanese dictionary
lookup results with compact reading-type and honorific-prefix metadata.
"""

from __future__ import annotations

import logging
import os
import sqlite3
import threading
from typing import Any, Dict, List, Optional, Set, Tuple

logger = logging.getLogger("ocr_server")

_KANA_HIRA_START = 0x3041
_KANA_HIRA_END = 0x3096
_KANA_KATA_START = 0x30A1
_KANA_KATA_END = 0x30F6

# Rendaku voicing map (unvoiced -> voiced)
_RENDAKU_MAP: Dict[str, str] = {
    "か": "が", "き": "ぎ", "く": "ぐ", "け": "げ", "こ": "ご",
    "さ": "ざ", "し": "じ", "す": "ず", "せ": "ぜ", "そ": "ぞ",
    "た": "だ", "ち": "ぢ", "つ": "づ", "て": "で", "と": "ど",
    "は": "ば", "ひ": "び", "ふ": "ぶ", "へ": "べ", "ほ": "ぼ",
}

# Handakuon map (h-row -> p-row, common in compounds after っ/ん)
_HANDAKUON_MAP: Dict[str, str] = {
    "は": "ぱ", "ひ": "ぴ", "ふ": "ぷ", "へ": "ぺ", "ほ": "ぽ",
}

# Entries in JMdict starting with お/ご/御 where the prefixed form has a distinct
# lexical meaning rather than being a productive polite/honorific form of the base word.
_HONORIFIC_SEMANTIC_SHIFT_BLOCKLIST: Set[Tuple[str, str]] = {
    ("中", "なか"),
    ("前", "まえ"),
    ("上", "かみ"),
    ("上", "うえ"),
    ("下", "した"),
    ("先", "さき"),
    ("元", "もと"),
    ("側", "そば"),
    ("代", "しろ"),
}

# Disqualifying JMdict misc/POS tags for honorific prefixing
_DISQUALIFYING_TAGS: Tuple[str, ...] = (
    "vulgar",
    "derogatory",
    "slang",
    "archaic",
    "obsolete",
    "onomatopoeic",
    "children's language",
    "historical",
    "rare",
    "jocular",
)

_NON_WORD_POS_TAGS: Tuple[str, ...] = (
    "[Name]",
    "[Unmatched]",
    "[OCR]",
    "Proper Noun",
    "Trainer Name",
    "Custom",
    "Pok",
)

# Tier 1: Curated high-precision お〜 entries.
# Includes (headword, reading_hira) pairs for homographs, and headword-only strings.
_CURATED_O_HOMOGRAPHS: Dict[Tuple[str, str], str] = {
    ("金", "かね"): "お〜",
    ("米", "こめ"): "お〜",
    ("釣り", "つり"): "お〜",
    ("茶", "ちゃ"): "お〜",
}

_CURATED_O_HEADWORDS: Set[str] = {
    # Everyday Kango exceptions taking お〜
    "茶", "天気", "電話", "時間", "勉強", "料理", "食事", "掃除", "洗濯", "散歩",
    "元気", "綺麗", "返事", "礼", "客", "世話", "邪魔", "風呂", "菓子", "肉",
    "野菜", "弁当", "財布", "給料", "化粧", "誕生日", "葬式", "通夜", "線香",
    "会計", "勘定", "稽古", "布団", "座敷", "正月", "盆", "寺", "経", "宅",
    "写真", "医者", "帽子", "洋服", "醤油", "砂糖", "豆腐", "寿司", "饅頭",
    "煎餅", "酢", "味噌", "団子", "茶碗", "上手", "下手", "得意", "利口",
    "達者", "大事", "粗末", "気軽", "気楽", "気の毒", "土産", "風邪", "怪我",
    "熱", "薬", "注射", "産", "中元", "歳暮", "香典", "賽銭", "神輿", "隣",
    "相手", "加減", "調子", "便所", "精進",
    # Everyday Wago nouns taking お〜
    "水", "湯", "酒", "魚", "餅", "皿", "箸", "鍋", "名前", "手紙", "葉書",
    "仕事", "部屋", "国", "里", "庭", "城", "店", "空", "山", "花", "話",
    "休み", "買い物", "出かけ", "帰り", "迎え", "見送り", "見舞い", "見合い",
    "祝い", "祭り", "参り", "守り", "願い", "詫び", "誘い", "断り", "尋ね",
    "問い合わせ", "知らせ", "伝え", "届け", "渡し", "預かり", "借り", "貸し",
    "返し", "待ち", "急ぎ", "疲れ", "体", "腹", "顔", "口", "足", "髪", "肌",
    "心", "気持ち", "気遣い", "心遣い", "心当たり", "着物", "履物", "鞄", "傘",
    "近く", "向かい", "暇", "好き", "嫌い", "小遣い", "年玉", "花見", "月見",
    "彼岸", "手洗い", "吸い物", "刺身", "漬物", "茶漬け", "代わり", "通し",
}

_CURATED_O_I_ADJECTIVES: Set[str] = {
    "忙しい", "若い", "美しい", "優しい", "早い", "高い", "安い", "強い",
    "詳しい", "寂しい", "恥ずかしい", "懐かしい", "近い", "遠い", "寒い", "暑い",
}

# Tier 1: Curated high-precision ご〜 entries (Wago exceptions + high-frequency Kango)
_CURATED_GO_HEADWORDS: Set[str] = {
    "家族", "両親", "主人", "兄弟", "姉妹", "夫婦", "親戚", "友人", "知人",
    "本人", "上司", "部下", "社員", "担当", "担当者", "代表", "社長", "部長",
    "課長", "住所", "出身", "職業", "専門", "趣味", "年齢", "予算", "都合",
    "事情", "関係", "理由", "目的", "意見", "感想", "親切", "丁寧", "立派",
    "無事", "自由", "不便", "不自由", "迷惑", "多忙", "多幸", "健康", "幸福",
    "幸運", "長寿", "冥福", "熱心", "優秀", "機嫌", "馳走", "贔屓", "無沙汰",
    "祝儀", "縁", "飯", "覧", "存知", "存じ", "一緒", "ゆっくり", "もっとも",
}


def _is_kanji(ch: str) -> bool:
    code = ord(ch)
    return (0x4E00 <= code <= 0x9FFF) or (0x3400 <= code <= 0x4DBF)


def _is_hiragana(ch: str) -> bool:
    return _KANA_HIRA_START <= ord(ch) <= _KANA_HIRA_END


def _is_katakana(ch: str) -> bool:
    return _KANA_KATA_START <= ord(ch) <= _KANA_KATA_END


def _to_hiragana(text: str) -> str:
    if not text:
        return ""
    out: List[str] = []
    for ch in text:
        code = ord(ch)
        if _KANA_KATA_START <= code <= _KANA_KATA_END:
            out.append(chr(code - 0x60))
        else:
            out.append(ch)
    return "".join(out)


def _has_katakana(text: str) -> bool:
    return any(_is_katakana(ch) for ch in text)


def _rendaku_variants(reading: str, allow_rendaku: bool, allow_handakuon: bool) -> List[str]:
    variants = [reading]
    if not reading:
        return variants
    first = reading[0]
    if allow_rendaku and first in _RENDAKU_MAP:
        variants.append(_RENDAKU_MAP[first] + reading[1:])
    if allow_handakuon and first in _HANDAKUON_MAP:
        variants.append(_HANDAKUON_MAP[first] + reading[1:])
    return variants


def _sokuon_variants(reading: str, followed_by_kanji: bool) -> List[str]:
    if not followed_by_kanji or len(reading) < 2:
        return []
    if reading[-1] in ("く", "き", "つ", "ち"):
        return [reading[:-1] + "っ"]
    return []


class JapaneseWordMetaAnalyzer:
    """
    Analyzes Japanese dictionary words to determine:
    1. Kanji reading type (音読み / 訓読み / 重箱読み / 湯桶読み / 音訓混用 / 熟字訓 / 当て字)
    2. Honorific/polite prefix applicability (お〜 / ご〜)
    """

    def __init__(self, jamdict_db_path: Optional[str] = None) -> None:
        self._db_path = jamdict_db_path
        self._lock = threading.Lock()
        self._loaded = False
        self._kanji_readings: Dict[str, Tuple[Set[str], List[Tuple[str, str]]]] = {}
        self._attested_honorifics: Dict[Tuple[str, str], str] = {}
        self._attested_honorifics_hw: Dict[str, str] = {}
        self._cache: Dict[Tuple[str, str, Tuple[str, ...], str], str] = {}

    def _ensure_loaded(self) -> None:
        if self._loaded:
            return
        with self._lock:
            if self._loaded:
                return
            self._load_from_db()
            self._loaded = True

    def _load_from_db(self) -> None:
        if not self._db_path or not os.path.isfile(self._db_path):
            return
        conn: Optional[sqlite3.Connection] = None
        try:
            try:
                conn = sqlite3.connect(
                    f"file:{self._db_path}?mode=ro", uri=True, check_same_thread=False
                )
            except Exception:
                conn = sqlite3.connect(self._db_path, check_same_thread=False)

            cur = conn.cursor()

            # 1. Load KANJIDIC2 on/kun readings
            readings_map: Dict[str, Tuple[Set[str], List[Tuple[str, str]]]] = {}
            cur.execute(
                """
                SELECT c.literal, r.r_type, r.value
                FROM character c
                JOIN rm_group g ON g.cid = c.ID
                JOIN reading r ON r.gid = g.ID
                WHERE r.r_type IN ('ja_on', 'ja_kun')
                """
            )
            for literal, r_type, val in cur.fetchall():
                if not literal or not val:
                    continue
                if literal not in readings_map:
                    readings_map[literal] = (set(), [])
                on_set, kun_list = readings_map[literal]
                clean = val.strip("-")
                if not clean:
                    continue
                if r_type == "ja_on":
                    hira = _to_hiragana(clean)
                    if hira:
                        on_set.add(hira)
                else:
                    hira = _to_hiragana(clean)
                    if "." in hira:
                        stem, okuri = hira.split(".", 1)
                    else:
                        stem, okuri = hira, ""
                    if stem:
                        pair = (stem, okuri)
                        if pair not in kun_list:
                            kun_list.append(pair)
            self._kanji_readings = readings_map

            # 2. Load JMdict attested polite/honorific お〜/ご〜/御〜 entries
            attested: Dict[Tuple[str, str], str] = {}
            attested_hw: Dict[str, str] = {}
            cur.execute(
                """
                SELECT k.text, kn.text,
                       GROUP_CONCAT(DISTINCT m.text) AS miscs,
                       GROUP_CONCAT(DISTINCT p.text) AS poses
                FROM Kanji k
                JOIN Kana kn ON k.idseq = kn.idseq
                JOIN Sense s ON s.idseq = k.idseq
                LEFT JOIN misc m ON m.sid = s.ID
                LEFT JOIN pos p ON p.sid = s.ID
                WHERE (k.text LIKE 'お%' OR k.text LIKE 'ご%' OR k.text LIKE '御%')
                  AND LENGTH(k.text) >= 2
                GROUP BY k.text, kn.text
                """
            )
            for k_text, kn_text, miscs, poses in cur.fetchall():
                if not k_text or not kn_text or len(k_text) < 2:
                    continue
                if not _is_kanji(k_text[1]):
                    continue
                miscs_l = (miscs or "").lower()
                if not any(t in miscs_l for t in ("polite", "honorific", "humble")):
                    continue
                poses_l = (poses or "").lower()
                if not any(
                    t in poses_l
                    for t in ("noun", "adjectival noun", "adjective (keiyoushi)", "suru")
                ):
                    continue
                if "pronoun" in poses_l and "noun (" not in poses_l:
                    continue

                base_hw = k_text[1:]
                rd_hira = _to_hiragana(kn_text)
                prefix: Optional[str] = None
                base_rd = ""
                if k_text.startswith("お") and rd_hira.startswith("お"):
                    prefix = "お〜"
                    base_rd = rd_hira[1:]
                elif k_text.startswith("ご") and rd_hira.startswith("ご"):
                    prefix = "ご〜"
                    base_rd = rd_hira[1:]
                elif k_text.startswith("御"):
                    if rd_hira.startswith("お") and not rd_hira.startswith("おん"):
                        prefix = "お〜"
                        base_rd = rd_hira[1:]
                    elif rd_hira.startswith("ご"):
                        prefix = "ご〜"
                        base_rd = rd_hira[1:]

                if not prefix or not base_rd:
                    continue
                if (base_hw, base_rd) in _HONORIFIC_SEMANTIC_SHIFT_BLOCKLIST:
                    continue
                attested.setdefault((base_hw, base_rd), prefix)
                attested_hw.setdefault(base_hw, prefix)

            self._attested_honorifics = attested
            self._attested_honorifics_hw = attested_hw
        except Exception as exc:
            logger.warning("JapaneseWordMetaAnalyzer failed to load %s: %s", self._db_path, exc)
        finally:
            if conn is not None:
                try:
                    conn.close()
                except Exception:
                    pass

    def _extract_jmdict_reading_flag(
        self, headword: str, reading: str, jmdict_entry: Any
    ) -> str:
        if jmdict_entry is None:
            return ""
        try:
            for knf in getattr(jmdict_entry, "kana_forms", []) or []:
                if getattr(knf, "text", "") == reading or not reading:
                    infos = " ".join(str(x).lower() for x in (getattr(knf, "info", []) or []))
                    if "gikun" in infos or "jukujikun" in infos:
                        return "熟字訓"
            for kf in getattr(jmdict_entry, "kanji_forms", []) or []:
                if getattr(kf, "text", "") == headword or not headword:
                    infos = " ".join(str(x).lower() for x in (getattr(kf, "info", []) or []))
                    if "ateji" in infos:
                        return "当て字"
        except Exception:
            pass
        return ""

    def classify_reading_type(
        self, headword: str, reading: str, jmdict_entry: Any = None
    ) -> str:
        """
        Classify whether a kanji-containing word is read with 音読み, 訓読み,
        重箱読み, 湯桶読み, 音訓混用, 熟字訓, or 当て字.
        Returns "" if headword has no kanji, reading is empty, or alignment is ambiguous.
        """
        if not headword or not reading:
            return ""
        if not any(_is_kanji(ch) for ch in headword):
            return ""

        special_flag = self._extract_jmdict_reading_flag(headword, reading, jmdict_entry)
        if special_flag:
            return special_flag

        self._ensure_loaded()
        if not self._kanji_readings:
            return ""

        rd_hira = _to_hiragana(reading)
        n_hw = len(headword)
        n_rd = len(rd_hira)

        memo: Dict[Tuple[int, int, Optional[str]], List[Tuple[Tuple[str, ...], int]]] = {}

        def _match_kanji(
            ch: str,
            hw_idx: int,
            rd_idx: int,
            prev_kanji: Optional[str],
        ) -> List[Tuple[int, str, int]]:
            """Return list of (next_rd_idx, 'on'|'kun', score_delta) for kanji at hw_idx."""
            lookup_ch = prev_kanji if (ch == "々" and prev_kanji) else ch
            entry = self._kanji_readings.get(lookup_ch)
            if not entry:
                return []
            on_set, kun_list = entry

            allow_rendaku = hw_idx > 0
            allow_handakuon = hw_idx > 0
            followed_by_kanji = (hw_idx + 1 < n_hw) and (
                _is_kanji(headword[hw_idx + 1]) or headword[hw_idx + 1] == "々"
            )
            next_hw_char = headword[hw_idx + 1] if (hw_idx + 1 < n_hw) else ""
            next_is_hira = bool(next_hw_char and (_is_hiragana(next_hw_char) or _is_katakana(next_hw_char)))
            next_hw_hira = _to_hiragana(next_hw_char) if next_is_hira else ""

            candidates: List[Tuple[int, str, int]] = []
            seen: Set[Tuple[int, str]] = set()

            def _add(end_idx: int, rtype: str, score: int) -> None:
                key = (end_idx, rtype)
                if key not in seen:
                    seen.add(key)
                    candidates.append((end_idx, rtype, score))

            # Try on'yomi readings
            for base_on in on_set:
                variants = _rendaku_variants(base_on, allow_rendaku, allow_handakuon)
                expanded: List[str] = []
                for v in variants:
                    expanded.append(v)
                    expanded.extend(_sokuon_variants(v, followed_by_kanji))
                for cand in expanded:
                    if rd_hira.startswith(cand, rd_idx):
                        _add(rd_idx + len(cand), "on", 0)

            # Try kun'yomi readings
            for stem, okuri in kun_list:
                stem_variants = _rendaku_variants(stem, allow_rendaku, allow_handakuon)
                expanded_stems: List[Tuple[str, int]] = []
                for sv in stem_variants:
                    expanded_stems.append((sv, 0))
                    for sk in _sokuon_variants(sv, followed_by_kanji):
                        expanded_stems.append((sk, 0))
                    # In compounds without okurigana (e.g. 受付 うけつけ, 入口 いりぐち),
                    # verb/adj stems often include the first mora of okurigana.
                    if okuri and followed_by_kanji:
                        expanded_stems.append((sv + okuri[0], -1))

                for cand_stem, bonus_adj in expanded_stems:
                    if not rd_hira.startswith(cand_stem, rd_idx):
                        continue
                    end_pos = rd_idx + len(cand_stem)
                    score = bonus_adj
                    if okuri:
                        if next_is_hira:
                            # Okurigana in headword confirms kun'yomi strongly
                            if next_hw_hira == okuri[0]:
                                score += 6
                            else:
                                # Also allow verb/adj conjugated/stem endings if stem matched
                                score += 2
                        elif followed_by_kanji:
                            score += 1
                    _add(end_pos, "kun", score)

            return candidates

        def _dp(hw_idx: int, rd_idx: int, prev_kanji: Optional[str]) -> List[Tuple[Tuple[str, ...], int]]:
            key = (hw_idx, rd_idx, prev_kanji)
            if key in memo:
                return memo[key]

            if hw_idx == n_hw:
                res = [((), 0)] if rd_idx == n_rd else []
                memo[key] = res
                return res

            ch = headword[hw_idx]
            out: List[Tuple[Tuple[str, ...], int]] = []

            if _is_kanji(ch) or (ch == "々" and prev_kanji is not None):
                next_prev = prev_kanji if ch == "々" else ch
                for next_rd, rtype, s_delta in _match_kanji(ch, hw_idx, rd_idx, prev_kanji):
                    for tail_types, tail_score in _dp(hw_idx + 1, next_rd, next_prev):
                        out.append(((rtype,) + tail_types, s_delta + tail_score))
            elif ch in ("ヶ", "ヵ"):
                for k_lit in ("か", "が", "け"):
                    if rd_hira.startswith(k_lit, rd_idx):
                        for tail_types, tail_score in _dp(hw_idx + 1, rd_idx + 1, prev_kanji):
                            out.append((tail_types, tail_score))
            elif _is_hiragana(ch) or _is_katakana(ch):
                ch_hira = _to_hiragana(ch)
                if rd_hira.startswith(ch_hira, rd_idx):
                    for tail_types, tail_score in _dp(hw_idx + 1, rd_idx + 1, prev_kanji):
                        out.append((tail_types, tail_score))
            elif ch == "ー":
                if rd_idx < n_rd and rd_hira[rd_idx] in ("あ", "い", "う", "え", "お", "ー"):
                    for tail_types, tail_score in _dp(hw_idx + 1, rd_idx + 1, prev_kanji):
                        out.append((tail_types, tail_score))
            else:
                # Unexpected symbol in headword: cannot reliably align
                out = []

            # Keep only top distinct type tuples to bound combinatorics
            best_by_types: Dict[Tuple[str, ...], int] = {}
            for t_tuple, sc in out:
                if t_tuple not in best_by_types or sc > best_by_types[t_tuple]:
                    best_by_types[t_tuple] = sc
            pruned = list(best_by_types.items())
            memo[key] = pruned
            return pruned

        alignments = _dp(0, 0, None)
        if not alignments:
            return ""

        # Score alignments: prefer okurigana-confirmed kun and uniform (all-on or all-kun) parses
        def _final_score(item: Tuple[Tuple[str, ...], int]) -> Tuple[int, int]:
            types, raw_score = item
            is_uniform = 1 if len(set(types)) == 1 else 0
            return (raw_score + (3 if is_uniform else 0), is_uniform)

        best_types, _ = max(alignments, key=_final_score)
        if not best_types:
            return ""

        unique_types = set(best_types)
        if unique_types == {"on"}:
            return "音読み"
        if unique_types == {"kun"}:
            return "訓読み"
        if len(best_types) == 2:
            if best_types == ("on", "kun"):
                return "重箱読み"
            if best_types == ("kun", "on"):
                return "湯桶読み"
        return "音訓混用"

    def classify_honorific_prefix(
        self,
        headword: str,
        reading: str,
        pos_notes: List[str],
        reading_type: str = "",
        jmdict_entry: Any = None,
    ) -> str:
        """
        Determine whether お〜 or ご〜 honorific/polite prefix is appropriate for this word.
        Returns "お〜", "ご〜", or "".
        """
        if not headword:
            return ""
        if headword[0] in ("お", "ご", "御", "オ", "ゴ"):
            return ""
        if reading and _has_katakana(reading):
            return ""
        if all(ord(c) < 128 or _is_katakana(c) or c == "ー" for c in headword):
            return ""

        joined_pos = " | ".join(pos_notes or [])
        for non_word in _NON_WORD_POS_TAGS:
            if non_word in joined_pos:
                return ""

        lower_pos = joined_pos.lower()
        notes_l = [p.lower() for p in (pos_notes or [])]
        is_vs = any("takes the aux. verb suru" in p for p in notes_l)
        is_noun = any(
            (
                "noun (common)" in p
                or "genitive case particle 'no'" in p
                or ("noun" in p and "prefix" not in p and "suffix" not in p and "pronoun" not in p and "adjectival" not in p and "pre-noun" not in p)
            )
            for p in notes_l
        )
        is_adj_na = any(
            ("adjectival nouns" in p or "keiyodoshi" in p or "na-adjective" in p)
            for p in notes_l
        )
        is_adj_i = any(
            ("adjective (keiyoushi)" in p or "i-adjective" in p)
            for p in notes_l
        )

        rd_hira = _to_hiragana(reading) if reading else ""

        def _clash_safe(pref: str) -> str:
            if pref == "お〜" and rd_hira.startswith("お"):
                return ""
            if pref == "ご〜" and rd_hira.startswith("ご"):
                return ""
            return pref

        # Tier 1: Curated high-precision lists (checked first)
        if (headword, rd_hira) in _CURATED_O_HOMOGRAPHS:
            return _clash_safe(_CURATED_O_HOMOGRAPHS[(headword, rd_hira)])
        if headword in _CURATED_O_HEADWORDS and (is_noun or is_vs or is_adj_na):
            # Avoid homograph false positive like 金 (きん)
            if headword == "金" and rd_hira and rd_hira != "かね":
                pass
            elif headword == "米" and rd_hira and rd_hira != "こめ":
                pass
            else:
                return _clash_safe("お〜")
        if is_adj_i and headword in _CURATED_O_I_ADJECTIVES:
            return _clash_safe("お〜")
        if headword in _CURATED_GO_HEADWORDS:
            return _clash_safe("ご〜")

        for disq in _DISQUALIFYING_TAGS:
            if disq in lower_pos:
                return ""

        if not (is_vs or is_noun or is_adj_na or is_adj_i):
            return ""

        # Tier 2: JMdict-attested お〜/ご〜/御〜 polite/honorific entries
        self._ensure_loaded()
        if is_noun or is_vs or is_adj_na:
            att = self._attested_honorifics.get((headword, rd_hira))
            if not att and not rd_hira:
                att = self._attested_honorifics_hw.get(headword)
            if att:
                return _clash_safe(att)

        # Tier 3: Productive 2+ kanji Kango suru-noun rule (ご〜)
        if (
            is_vs
            and reading_type == "音読み"
            and len(headword) >= 2
            and all(_is_kanji(c) for c in headword)
        ):
            return _clash_safe("ご〜")

        return ""

    def format_meta_tag(
        self,
        headword: str,
        reading: str,
        pos_notes: List[str],
        jmdict_entry: Any = None,
    ) -> str:
        """
        Return formatted bracketed tag combining reading type and honorific prefix,
        e.g. '[音読み · ご〜]', '[訓読み · お〜]', '[音読み]', '[訓読み]', '[ご〜]', or ''.
        """
        if not headword:
            return ""
        special_flag = self._extract_jmdict_reading_flag(headword, reading, jmdict_entry)
        notes_tuple = tuple(pos_notes or ())
        cache_key = (headword, reading or "", notes_tuple, special_flag)
        with self._lock:
            if cache_key in self._cache:
                return self._cache[cache_key]

        reading_type = (
            special_flag
            if special_flag
            else self.classify_reading_type(headword, reading, jmdict_entry=jmdict_entry)
        )
        honorific = self.classify_honorific_prefix(
            headword,
            reading,
            pos_notes,
            reading_type=reading_type,
            jmdict_entry=jmdict_entry,
        )

        parts: List[str] = []
        if reading_type:
            parts.append(reading_type)
        if honorific:
            parts.append(honorific)
        tag = f"[{' · '.join(parts)}]" if parts else ""

        with self._lock:
            if len(self._cache) < 8192:
                self._cache[cache_key] = tag
        return tag
