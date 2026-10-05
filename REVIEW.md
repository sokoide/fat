# FAT実装 コードレビュー報告と改善計画

日付: 2026-10-05
方法: 4エージェント並列レビュー（仕様準拠 / メモリ安全性 / 設計 / ビルド・テスト）+ clang 21によるASan+UBSan実証。破損イメージ7種とミニハーネスで再現確認（成果物: `/tmp/fatreview/`）。

---

## 総評

FAT12のコアデコード（`fat_get_fat`の12bit展開・`DirectoryEntry`構造体レイアウト・領域導出）は仕様どおりで正しい。一方で (1) 削除エントリとチェーン終端の走査規則が仕様違反、 破損イメージに対する防御がゼロで大半が即SEGV、 グローバル状態＋モデル/ビュー同居という構造的負債が高度化をブロック、の3層の問題がある。更にレビュー時点で`make test`は失敗状態だった。

## 0. 当時進行中の障害

| 状況 | 根源 |
|---|---|
| `make test`がexit 134で失敗 | `demof12.fat`がmtools 4.0.48再生成でOEM=`MTOO4048`。`testmain.c:13`は`MTOO4032`を期待 → OEMアサートは削除する（ジオメトリassert群が実質をカバー） |
| `.gitignore`が機能せず`tags`がgit管理下 | `tag`は`tags`のtypo |

## 1. 仕様違反（正確性）

- **H1. 削除エントリで走査打ち切り** — `fat.c:340` `fat.c:366` `fat.c:308` `testmain.c:74` `testmain.c:107`: `name[0]==0xE5`は`continue`が正しい（0x00のみ走査終了）。削除履歴のあるイメージでls結果が欠ける。
- **H2. EOC判定のハードコード `cluster < 0xF00`** — `fat.c:298` `fat.c:358` `testmain.c:99`: 正しい`fat_is_end_of_cluster`（`fat.c:474`）が未使用・`fat.h`に宣言なし。0xF00–0xFF7（予約/bad）を正常終端扱い、の大容量FAT12でチェーン途中打ち切り、FAT16/32では全く誤り。
- **M5. `main.c:135-137` 行順序バグ** — `ls /dir2/subbdir1`（typoも）がdir2のリストを表示。137行目はdead store。
- **M2. `fat_print_fat12`ループ上限`fat.c:192`** — `tableCount`（FAT個数）を`tableSize16`（FATセクタ数）と取り違え、FAT全1024エントリ中341件のみ表示。
- **M1. FATタイプ判定 `fat.c:60-67`** — `totalSectors32`未参照、閾値は`< 4085`が仕様（`<=`は不一致）、FAT16/32と判定されても`fat_get_fat`は常時12bitデコード。未対応なら明示エラーに。
- **M3. LFN未処理** — 属性0x0Fエントリが`callback_ls`（`main.c:31`）で生バイトを「V」として表示。両イテレータでスキップが必要。
- **M4. パス解決 `main.c:50-62`** — 中間コンポーネント不在時、戻り値0がルートと衝突し以降トークンをルートから再探索。空ファイル（正当にcluster 0）が"path not found"。センチネル0の二義性が根源。
- **低** — 拡張子末尾スペース未除去（`FOO.A  `）、0x05頭バイト（漢字）未対応、`'.'`/`'..'`解決不能、`fat_print_info`の切り捨て除算。

**検証済み正常**: `fat_get_fat`偶数/奇数の12bit復号、`DirectoryEntry`オフセット（sizeof=32）、`FatBS`先頭36バイト、ルートdirセクタ切上げ（`fat.c:56-58`）、`(attr & 0x18) == 0`の拡張子判定。

## 2. メモリ安全性（ASan/UBSan実証済み。正常イメージでは無検出）

| # | 箇所 | シナリオ |
|---|---|---|
| 1 | `fat.c:37,44` | `totalSectors16*bytesPerSector`のintオーバーフロー（UBSan実証）→size_tラップ→巨大malloc（ASan実証） |
| 2 | `fat.c:53-61` | 領域計算無検証。`reservedSectorCount=0xFFFF`のみでinit成功→SEGV |
| 3 | `fat.c:401-418` | `fat_set_entry_name`が境界なし書き込み。ベース>8/拡張>3で`name[11]`超過（スタック破壊実証） |
| 4 | `fat.c:524` | 非NUL終端`name[11]`への`strlen`（heap外読取33B実証） |
| 5 | `main.c:69-85` | `cat_file_for_cluster`にEOC検査なし→fileSize水増しのみでバッファ外memcpy SEGV（実証）。1024固定もクラスタサイズ≠1024で破綻 |
| 6 | `fat.c:358`等 | サイクル検出なし。FAT[8]=5,[5]=6,[6]=5で無限ループ（実証） |
| 7 | `fat.c:26-27,50` | init失敗時`_fat_bs`が死亡スタックフレームを指す→SEGV（実証）。`fat_unint`は`_fat_bs`をNULL化せず |

**中:** 再init時リーク（`main.c`は`fat_unint`未呼び出し）、`strcpy(tmp_path[64])`（`main.c:44`）、`fat_get_entry_name`の`len<12`は1バイト足りない（8.3名は13バイト必要→`len<13`）、0x55AA未検証（ゼロ埋めでもinit成功・実証）、クラスタ0/1/freeで`(cluster-2)`がuint32ラップ→FAT領域を誤解釈（実証）。

**低:** `_fat_buffer`/`_fat_bs`が非staticで外部リンケージ（duplicate symbol実証）、`fat.h`に`fat_is_*`等のプロトタイプ欠落、`void*`算術はGNU拡張。

**安全確認済み:** `bytesPerSector=0`は除算到達前に`fread`失敗で拒否（エラーメッセージは誤解招く）、二重`fat_unint`は安全、`fread(512,1)`の部分読みなし。

## 3. 設計 — 高度化の阻害要因

1. **グローバル可変状態**（`fat.c:14-22`）→ `fat_ctx_t *ctx`を全関数第一引数に。複数イメージ・テスト独立性・解放順序問題が解消。
2. **モデル/ビュー同居** — `fat.c`の47%が`fat_print_*`族、コアがcolor.hに依存する逆流 → `fat_core.c`（stdio/color依存ゼロ）/`fat_dump.c`に分割。
3. **エラー処理** — ライブラリ内`fprintf(stderr)`+bool → `fat_result_t` enum + `fat_strerror()`。M4のセンチネル二義性の根本解: `fat_result_t fat_lookup(ctx, dir, path, fat_dirent_t *out)`。
4. **隠れた副作用** — `fat_get_cluster_for_entry`が検索なのに呼び出し元entryを上書き。
5. **命名** — `fat_unint`→`fat_uninit`、`ignoreInFAT12`→`firstClusterHigh`、`startingClusterNumber`→`firstClusterLow`、`uintN_t`統一。

**デッドコード:** `fat.c:280-292`の到達不能ルート分岐、未使用の`check_null`/`clbg`/`cl_test`/`spike_strtok`、コメント塊。`FatExtBS32`のコメント块のみFAT32対応のため温存。

## 4. パフォーマンス

- 丸ごとRAM載せは720KBでは可、FAT32数GBで破綻 → フェーズ5で解決。
- `_callback_find_entry`は一致後も走査が止まらない → 停止契約へ。
- ダンプ系の1バイト毎`printf`。`cat_file_for_cluster`の`%s`はNUL打ち切り → バイナリ安全な読み出しAPIの欠如。

## 5. ビルド・テスト

**Makefile（実証済み）:** ヘッダ依存追跡なし（`touch fat.h`で再コンパイルゼロ）→ `-MMD -MP`+`-include`。`$(TARGET): build`パターンで毎回再リンク。`OUTDIR = ~/tmp/hoge`はリポジトリ外共有ディレクトリで`clean`が他の作業を消す → `OUTDIR ?= build`。`rm $(IMG)`に`-f`なし。`CXX`→`CC`。`-Wextra -Wshadow`は警告12件のみで導入容易。

**テスト:** `cat_file`のマルチクラスタ読み（`test_5kb.txt`=4962B=5クラスタのfixtureあり）と`cluster_for_path`がゼロテスト。assertのみで進行表示なし → `RUN()`マクロ。fixtureは「コミット済みかつ再生成可能だが非再現」の矛盾 → 凍結+CI diff か untrack+生成依存 のどちらかへ。クラスタ8/11等のマジックナンバーは1テストに集約。

**ツーリング:** ASan/UBSanテストターゲット（正常fixtureでgreen実証済み）、`make check`、`scan-build`、mtoolsをオラクルに`mdir -b`/`mtype`との差分assert、ブートセクタbit反転マイクロファズ。

## 6. 高度化ロードマップ

- **フェーズ0（全前提）:** コンテキスト化、`fat_core`/`fat_dump`分割、エラーenum、`fat_dirent_t`（`name[256]`でLFN前提）、センチネル整理（`FAT_CLUSTER_ROOT`明示的命名）。
- **フェーズ1 堅牢性:** BPB検証（`bytesPerSector∈{512,1024,2048,4096}`、`sectorsPerCluster`2の冪、`totalSectors16==0`なら`totalSectors32`、積≤実ファイルサイズ、0x55AA）、範囲検査、`fat_is_end_of_cluster`統一+反復上限=クラスタ数。
- **フェーズ2 FAT16/32:** `fat_get_fat`タイプ別ディスパッチ（FAT32は`& 0x0FFFFFFF`マスク必須）、FAT32ルートはBPB[44]`rootCluster`のチェーン走査、`FatExtBS32`実体化、FSInfo読み取り、アクティブFAT選択。
- **フェーズ3 LFN:** 0x0F連続エントリ結合、UTF-16→UTF-8、短名チェックサム検証、LFN優先・8.3フォールバック。
- **フェーズ4 書き込み:** FAT書き込み（12bitはread-modify-write、両FATコピー）、クラスタ確保/解放、ディレクトリスロット探索、FSInfo更新、DOSタイムスタンプ。
- **フェーズ5 I/O抽象化:** `fat_io_t{read/write/size}`差し替え+セクタキャッシュ。丸ごとRAMは`fat_io_mem`実装の一つに。FILE*・ブロックデバイス・FUSEバックエンドが並列実装可能。

## 7. 修正優先順位

1. **即時:** OEMアサート削除（`make test`復活）、`.gitignore`の`tags`修正
2. **小差分の実害解消:** 0xE5 continue → `fat_is_end_of_cluster`使用 → `main.c`行順序 → `len<13` → LFNスキップ
3. **防御:** BPB検証・範囲検査・`fat_set_entry_name`境界・`strlen`→バイト比較・サイクル上限、`-fsanitize`ターゲットと`-MMD`
4. **フェーズ0リファクタ** → 以降フェーズ順（別途承認）

## 8. 修正ウェーブ1の分担とAPI契約（本日実施）

ファイル所有権で衝突を回避（`fat.c`/`fat.h`、`main.c`、`Makefile`/`testmain.c`/`color.*`/`.gitignore`）。

**`fat.h` の新規公開契約（全員がこの署名に合わせる）:**

```c
#define FAT_CLUSTER_ROOT 0u
#define FAT_CLUSTER_NOT_FOUND 0xFFFFFFFFu

void  fat_uninit(void);                      /* fat_unintから改名。_fat_bs等もNULL化 */
bool  fat_is_broken(uint32_t cluster);
bool  fat_is_end_of_cluster(uint32_t cluster);
uint32_t fat_get_cluster_size(void);         /* bytesPerSector * sectorsPerCluster */
bool  fat_set_entry_name(DirectoryEntry* entry, const char* name); /* 8.3非表現はfalse */
uint32_t fat_get_cluster_for_entry(uint32_t parent_cluster, DirectoryEntry* entry);
                                            /* 不一致時 FAT_CLUSTER_NOT_FOUND */
void* fat_get_cluster_ptr(uint32_t cluster); /* 範囲外は NULL */
```

- fix-core: `fat.c`/`fat.h` — H1/H2/M2/M3、安全性防御（BPB検証・範囲検査・サイクル上限・`len<13`・`strlen`排除・`fat_set_entry_name`境界・static化・`fat_unint`改名・デッドコード削除）、上記API契約の実装、`-Wextra`警告の解消
- fix-app: `main.c` — M5行順序、`tmp_path`境界、`cat_file_for_cluster`のEOC/クラスタサイズ/`fwrite`化、`FAT_CLUSTER_NOT_FOUND`扱い、中間コンポーネントのATTR_DIRECTORY検証、`fat_uninit`呼び出し
- fix-build: `Makefile`/`testmain.c`/`color.c|h`/`.gitignore` — 依存追跡・OUTDIR→`build`・再リンク解消・`rm -f`・`CC/CFLAGS`・`test-san`(ASan/UBSan)・`check`ターゲット、OEMアサート削除・`RUN()`マクロ・`fat_is_end_of_cluster`への置換・多クラスタ読みテスト追加、`cl_test`/`clbg`/`spike_strtok`削除
- 統合検証（fix-complete）: 全ファイル揃った後に`make check`・ASan・破損イメージ再実験（`/tmp/fatreview`）

コミットはユーザー指示まで行わない。

## 9. ウェーブ1実施結果（2026-10-05 完了）

全チェック合格。最終状態:

- `make check`: クリーンビルド警告ゼロ（`-Wall -Wextra -Wshadow`）、テスト9件×2（通常+ASan/UBSan）全ok
- デモ: `ls /dir2/subdir1`がsubdir1自身の内容を表示（M5解消）、catはバイナリ安全、破損時はSEGV而非ずstderr警告で停止
- 破損イメージ回帰（ASan+UBSan、報告ゼロ）: 0x55AA破壊/rsv=0xFFFF/totalSectors16=0はinitが明確メッセージで拒否、FATサイクル8↔9は走査停止、fileSize水増しは警告+正常終了、`fat_get_cluster_ptr(0/1/範囲外)`はNULL
- 増分ビルド: 変更なし再実行で「Nothing to be done」、`touch fat.h`で依存再コンパイル（`.d`追跡動作）
- 統合時の追加修正: testmain.c 3件（チェーン長カウントのオフバイワン、最終部分クラスタ未読、未初期化`DirectoryEntry`のUB）+ リード作業3件（未使用include除去`fat.c`/`main.c`、`Circular build`警告解消のため未使用`build`エイリアル削除）

残課題（フェーズ0以降または要ユーザー判断）:
- `tags`はgit追跡済みのまま（ignoreは追跡ファイルに効かない）。解除には`git rm --cached tags`+コミットが必要
- `demof12.fat`はウェーブ前にmtools 4.0.48で再生成済み（OEM=MTOO4048）。fixture戦略（凍結+CI diff か untrack+生成依存）は未決定
- FATタイプ閾値`< 4085`化、再init失敗時の旧バッファ残存、`fat_get_fat`の引数範囲検査、0x05頭バイト、`'.'`/`'..'`解決 → フェーズ1/2へ繰り越し
