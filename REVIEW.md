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
- **フェーズ4 書き込み:** FAT書き込み（12bitはread-modify-write、両FATコピー）、クラスタ確保/解放、ディレクトリスロット探索、FSInfo更新、DOSタイムスタンプ。詳細設計は§12。
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
- FATタイプ閾値`< 4085`化、再init失敗時の旧バッファ残存、`fat_get_fat`の引数範囲検査、0x05頭バサイズ、`'.'`/`'..'`解決 → フェーズ1/2へ繰り越し

## 10. フェーズ0実施結果（2026-10-05 完了）

アーキテクチャ: `fat.h`（公開契約: `fat_ctx_t`不透明・`fat_result_t`・`fat_dirent_t`(`name[256]`)・`fat_geometry_t`）/ `fat_internal.h`（ディスク上構造体・`struct fat_ctx`・ATTR_*、ライブラリ内専用）/ `fat_core.c`（stdio・color・ファイルスコープ変数ゼロ）/ `fat_dev.c`（`fat_open`のみ、stdioはここだけ）/ `fat_dump.c`（`fat_print_*`、`const fat_ctx_t*`）。`fat.c`は削除。

達成: コンテキスト化（複数イメージ・`fat_close(NULL)`安全・コアのグローバル状態ゼロ）、エラーenum+`fat_strerror`（ライブラリ内印刷廃止）、モデル/ビュー分離、`fat_open_mem`（メモリ変異テスト: 0x55AA破壊・FAT自己ループ・fileSize水増し→`BAD_CLUSTER`等、外部ハーネス不要）、`fat_lookup`（`.`/`..`解決、`NOT_FOUND`/`PATH_NOT_FOUND`区別）、`fat_read_file`（バイナリ安全・所有権明確）、FATタイプ閾値のspec準拠化（`< 4085`）、`totalSectors16==0`→`FAT_ERR_UNSUPPORTED`。

検証: `make check` = 9テスト×2（通常+ASan/UBSan）全ok・警告ゼロ。デモ全セクション正常（新規`cat /dir1/../hello.txt`、`test_5kb.txt`4962バイト完全一致、クラスタ直書き廃止）。

未決の契約判断: ルート直下の`.`のlookupは`FAT_ERR_PATH_NOT_FOUND`（`..`と対称・実装側）。DOS流にrootの合成direntで`FAT_OK`にする選択も可（1行変更）。

フェーズ候補のnotes: 公開APIのconst一貫性（`fat_dump.c`のconst外し1箇所）、`fat_print_directory_entry`が実質デッドexport、`-Wstrict-prototypes`対応、dumpの色サイクルstatic（フェーズ5再入化時）。

## 11. フェーズ2実施結果（2026-10-05 完了）

チーム編成: リードが`fat.h`契約を先に拡張（`fat_geometry_t.root_cluster`、`fat_fsinfo_t`+`fat_fsinfo()`、FAT16/32サポート明記）→ p2-lib（`fat_internal.h`/`fat_core.c`/`fat_dump.c`）、p2-test（`testmain.c`/`Makefile`/`.gitignore`）、p2-app（`main.c`改名追従）の並列分担。p2-lib/p2-testはAPI使用制限（429）でレポート送信直前に中断したが、ファイル書き込みは完了していたため、リードが統合検証を直接実施した。

達成:
- **タイプ別FATデコード**: FAT12従来/ FAT16 u16 / FAT32 u32 `& 0x0FFFFFFF`（マスク必須）。エントリ読取り前にタイプ別バイト長でFAT領域内を検証。EOC/bad/reserved閾値もタイプ別（0xFF8系/0xFFF8系/0x0FFFFFF8系）。
- **totalSectors32対応**: totalSectors16==0なら32bit値（従来の`UNSUPPORTED`撤廃）。fatSizeもtableSize16==0ならfatsz32。領域導出は全て64bit中間計算。
- **FAT32ルート=チェーン走査**: `FatExtBS32`実体化（extended_section[0]、common tailは[28]）。`rootClus`検証（2..cluster_count+1）、`geo.root_cluster`に反映、`root_dir_sector/sectors`は0。`fat_iter_dir(FAT_CLUSTER_ROOT)`はタイプで自動ディスパッチ。
- **active FAT選択**: extFlags bit7でミラー無効時、下位4bitのFAT番号を`fat_start_sector`に反映（範囲外は`INVALID_BPB`）。
- **FSInfo**: `fat_fsinfo()`。署名3種（RRaA/rrAa/0xAA550000）検証、不正・不在は0xFFFFFFFF（unknown）で`FAT_OK`、FAT12/16は`UNSUPPORTED`。
- **firstClusterHigh/Low合成**: FAT32は32bit合成、FAT12/16はlowのみ。
- **dump**: `fat_print_fat12`→`fat_print_fat`改名（%03X/%04X/%08X、FAT32は先頭1024エントリ+打ち切り表示）、`fat_print_info`のタイプ対応、FAT32 EBPBダンプ。

フィクスチャ（mtools実物、gitignore、`make fat16`/`make fat32`で生成、`check-all`=生成+check）:
- `demof16.fat` 16MiB: cluster 32481、チェーン3→..→12、FAT[0]=0xFFF8
- `demof32.fat` 33MiB: cluster 66512、**ルートが3クラスタ chain 2→54→55（44エントリ）**、FSInfo free=66454（mdir 34024448B freeと一致検証済み）

検証: `make check` = **19テスト×2**（FAT12 9 + FAT16 4 + FAT32 6、通常+ASan/UBSan）全ok・警告ゼロ。fixture不在時はスキップ表示で`check`は高速維持。デモはdemof12で従来どおり全セクション正常。

統合時にリードが修正したバグ1件: `fat_read_file`のチェーンループ検出は「サイズ不足で飢えるループ」しか捕捉せず、自己ループ（FAT[5]=5）はfile_size分をゴミで埋めて`FAT_OK`になる契約違反 → **Brentのサイクル検出**（O(1)空間、1ステップ1比較）をreadウォークに追加し「loops→BAD_CLUSTER」契約を遵守。

残課題: FAT16/32のOEM・fatsz整合の追加mutation、ルート直下の`.`のlookup挙動（§10未決のまま）、フェーズ3 LFN（0x0F連続エントリ）は現在スキップのみ。

## 12. フェーズ4（書き込み）詳細設計（2026-10-05 追記）

前提: フェーズ0〜2完了（コンテキスト化・エラーenum・FAT12/16/32デコード）。イメージは`fat_open`/`fat_open_mem`で丸ごとRAM載せのため、フェーズ4の書き込みはメモリ上のイメージコピーへの更新とし、永続化は`fat_write()`（全バッファをオリジナルパスへ書き戻し）で行う。I/O抽象化（フェーズ5）でこの境界を`fat_io_t`に差し替える。

### 12.1 FATエントリ書き込み `fat_set_fat_entry`

- 12bitはバイト非アライメント。オフセット`cluster*3/2`の2バイトを既にFAT12では1バイト読みの`fat_get_fat_entry`で処理済みだが、書き込みは必ず**read-modify-write**（隣接エントリとニブルを共有するため、隣を壊さない）:
  - 偶数: `b[0] = value & 0xFF`、`b[1] = (b[1] & 0xF0) | (value >> 8)`
  - 奇数: `b[0] = (b[0] & 0x0F) | (value << 4)`、`b[1] = value >> 8`
- **両FATコピーへ書き込む**: `fat_count`分のテーブル先頭オフセットをループ。ミラー間不一致は書き込み後に一致検証するか、少なくとも既存チェーン読み取りと同じactive FAT選択規則に従う。
- 値はタイプ別に妥当化: EOCマークはタイプ別定数（0xFFF / 0xFFFF / 0x0FFFFFFF）、FREEは0。`cluster`は2..cluster_count+1のみ（0/1はINVALID_ARG）。
- FAT32は既存どおり`& 0x0FFFFFFF`マスク（上位4bitは予約のため保存）。

### 12.2 クラスタ確保 `fat_alloc_cluster`

- FAT[2..]を先頭から走査しFREE（0）を探す。FAT32はFSInfoの`next_free_cluster`ヒントから試み、その値が本当にFREEのときのみ採用（stale=0xFFFFFFFFや誤値は無視して全走査）。
- 見つけたらEOCマークを書き込み、FSInfoのfree数を減算。空きなしは新enum `FAT_ERR_DISK_FULL`。
- 確保後はチェーン接続は呼び出し側の責務（`fat_set_fat_entry(prev, new)`）。

### 12.3 クラスタ解放 `fat_free_chain`

- 先頭からEOCまで辿って各エントリをFREEに。サイクル検出は`fat_read_file`に追加済みのBrent法と同じ上限（cluster_count反復）で防御。
- FSInfoのfree数を加算し、`next_free_cluster`ヒントを先頭クラスタに更新（or unknown化）。

### 12.4 ディレクトリスロット探索 `fat_add_dirent`

- 0x00=未使用（以降も0x00が続くため、ここから連続枠を取れる）、0xE5=削除済み（単独で再利用可）。
- フェーズ4は8.3のみでスロット1個。フェーズ3 LFN結合後は「短名+LFN N個」の連続N+1枠が必要（0x40フラグ・チェックサムはフェーズ3で設計）。
- FAT12/16のルートは固定領域（`root_entries`上限、拡張不可）→ 満杯は新enum `FAT_ERR_DIR_FULL`。
- サブディレクトリとFAT32ルートはチェーン走査し、空き枠がなければ`fat_alloc_cluster`でチェーンを延長し、新クラスタを0x00でゼロ埋め（0x00=終端マーカーを保証）。

### 12.5 FSInfo更新（FAT32のみ）

- 確保/解放に応じて`free_cluster_count`±1、`next_free_cluster`書き換え。署名不正・不在（0xFFFFFFFF）のときは壊さずunknownのまま維持（§2の`fat_fsinfo()`契約と対称）。

### 12.6 DOSタイムスタンプ

- FatDate = `((year-1980)<<9) | (month<<5) | day`、FatTime = `(hour<<11) | (min<<5) | (sec/2)`（2秒粒度、`creation_time_tenth`が追加精度）。
- 作成時: `creation_date/time` + `last_write_date/time` + `last_access_date`（FAT32慣例）。更新時: `last_write_*`のみ。
- タイムゾーンは実装方針を明記（DOS流ローカルタイム or UTC、どちらでもよいが統一）。

### 12.7 書き込み順序（失敗時一貫性）

クラッシュ耐性はRAM載せ+全体flushのため題外だが、**操作途中の失敗で不整合を残さない**順序にする:

1. データ書き込み先クラスタを確保（チェーン構築）→ 2. データ全量を書き込み → 3. 最後にdirentのfirst cluster / file_sizeを更新。

どのステップで失敗しても、確保済みチェーンを`fat_free_chain`で解放して`FAT_ERR_*`を返す。direntが最後に変わるため、失敗時に「direntだけ有効でデータ無し」の孤立参照を作らない。

### 12.8 公開API契約案（`fat.h`追記）

```c
fat_result_t fat_set_fat_entry(fat_ctx_t* ctx, uint32_t cluster, uint32_t value);
fat_result_t fat_alloc_cluster(fat_ctx_t* ctx, uint32_t* out);
fat_result_t fat_free_chain(fat_ctx_t* ctx, uint32_t head);
fat_result_t fat_add_dirent(fat_ctx_t* ctx, uint32_t dir_cluster,
                            const char* name, const fat_dirent_t* tmpl);
fat_result_t fat_write_file(fat_ctx_t* ctx, uint32_t dir_cluster,
                            const char* name, const uint8_t* data, size_t size);
                            /* 12.7の順序で alloc→write→dirent更新 */
fat_result_t fat_write(fat_ctx_t* ctx, const char* path); /* 全イメージflush。NULL=open元パス */
```

enum追記: `FAT_ERR_DISK_FULL`（FAT空きなし）、`FAT_ERR_DIR_FULL`（ディレクトリ枠なし）。

### 12.9 テスト計画

- mtoolsオラクル差分: 書き込み後に`mdir -b`/`mtype`で目録・内容一致assert（§5方針の継続）。
- 12bitニブル隣接保護: クラスタ3書き込み後、2/4の値が不変であること（偶数/奇数両方）。
- 両FATコピーの一致、FAT12/16ルート満杯、0xE5再利用、FAT32ルートチェーン延長、DISK_FULL。
- 書き込み→`fat_open`再読み込みでround-trip一致。全ターゲットを`test-san`（ASan/UBSan）に追加。

## 13. ウェーブA&B: 正確性の詰め + ライブラリAPI（2026-10-05 計画、TDD実施）

LFN（フェーズ3）は見送り。読み取り専用ライブラリとしての完成度を上げる。**TDD**: リードが`fat.h`契約を先に拡張（完了）→ テストエージェントが全テストを先行記述しredを確認（この間ライブラリはコミット済みの安定状態）→ 実装エージェントがgreen化 → リードが最終検証。

### 13.1 契約変更（fat.h拡張済み）

- **A2 ルート直下の`.`/`..`**: DOS流に**合成direntで`FAT_OK`**（name="."/".."、ATTR_DIRECTORY、first_cluster=FAT_CLUSTER_ROOT、size=0）。§10の未決を決着。
- **A3 ボリュームラベル非マッチ**: `fat_lookup`はATTR_VOLUME_IDエントリにマッチしない（DOS open()準拠）。
- **A1 0x05頭バイト**: `fat_name_from_83`はname11[0]==0x05を0xE5として描画（日本語名エスケープ）。
- **B6 ストリーミング読み**: `fat_file_t`（opaque）+`fat_file_open/seek/read/tell/size/close`。全丸ごとmallocの`fat_read_file`はFAT16の2GBで破綻するため。seekはO(チェーン長)、readはEOF以外ショートなし、loopは`fat_read_file`と同じBrent防御で`BAD_CLUSTER`。
- **B7 ディレクトリカーソル**: `fat_dir_t`（opaque）+`fat_dir_open/next/close`。終端は新enum **`FAT_ERR_END_OF_DIR`**（継続呼び出しも同値）。`fat_iter_dir`は内部でcursor実装に統合（走査ロジックの二重化を防ぐ）。
- **B8 タイムスタンプ**: `fat_dos_date_to_tm(dos_date, dos_time, dos_tenth, struct tm*)`。範囲外フィールドは`INVALID_ARG`。タイムゾーンは中立（tm_isdst=0）。
- `fat_strerror`にEND_OF_DIR分を追加（リードが基盤として先行投入）。

### 13.2 テスト追加（p-ab-test、testmain.c/Makefile所有）

- A1: 0x05レンダリング単体 + 8.3ラウンドトリップ
- A2: `.`/`..` at root → FAT_OK + 合成dirent検証、`dir1/../..` の追跡
- A3: `"demof12"`/`"demof16"`/`"demof32"` lookup → NOT_FOUND
- B6: 全ファイル読み（`fat_read_file`とのバイト一致）、seek+tell、境界（0/size/size+1）、空ファイル、ショートしないread、FAT32の多クラスタ、突然のBAD_CLUSTER（mutation）
- B7: `fat_iter_dir`との列挙一致（3フィクスチャ）、終端後の継続呼び出し、root/FAT32ルートチェーン
- B8: DOS日時デコード（境界値1980/2107、月0/13、日0、時24、tenth）
- **A4 ブートセクタファズ**: FAT12は512B全bit反転（4096变异、open→iterate→lookup→readの生存）、FAT16/32はBPBフィールド値ベースmutation（{0x00,0xFF,0x55}×主要オフセット。イメージコピー34MB×4096は実行時間非現実的のため）。ASanビルドで実行。
- **A5 mtools差分**: `mdir -b`出力と当方の列挙（名前/サイズ/種別）を3フィクスチャ横断で比較、`mtype`と`fat_read_file`のバイト一致。mtools不在時はスキップ表示。
- Makefile: `-Wstrict-prototypes`追加、ファズ・差分を`check`に含めつつ実行時間を概算1分以内に。

### 13.3 実装（p-ab-lib、fat_internal.h/fat_core.c/fat_dump.c所有）

- A1/A2/A3: `fat_name_from_83`/`fat_lookup`（合成dirent生成、label除外）
- B6: `struct fat_file`（ctx借用+direntコピー+位置/クラスター状態+Brent状態）
- B7: `struct fat_dir`（走査状態）で`fat_iter_dir`を再実装（同一ロジック共用）
- B8: ビット分解デコーダ
- **A9 掃除**: `fat_dump.c`のconst外し解消、`fat_print_directory_entry`デッドexport削除、`-Wstrict-prototypes`対応
- 制約: コアはstdioゼロ・グローバル状態ゼロ・警告ゼロを維持。テストファイルは触らない（テストの誤りはリードにエスカレーション）

### 13.4 検証

redリスト記録 → green化 → リードが`make check`（+ASan）とデモを直接実行、§14に結果記録。コミットは指示待ち。

## 14. ウェーブA&B実施結果（2026-10-05 完了）

TDD実施: **p-ab-test**がred フェーズで全テスト先行記述（A1/A2/A3はアサーションred、B6/B7/B8はリンクred=未定義シンボル10個、A4ファズ/A5 mtools差分は現行libで緑の回帰ロック。red理由はプローブ実測で「契約未実装」であることを全件確認）→ **p-ab-lib**がgreen化（凍結テスト・契約fat.h変更なし、指摘ゼロで一発green）→ リード検証。

達成:
- **A1**: `fat_name_from_83`はname11[0]==0x05を0xE5描画
- **A2**: ルート直下の`.`/`..`は合成dirent（name/ATTR_DIRECTORY/`FAT_CLUSTER_ROOT`/size 0）で`FAT_OK`。§10の未決解消。非ルートの`.`は実ディスク"."エントリ解決（相対開始クラスタ対応）
- **A3**: `fat_lookup`はATTR_VOLUME_ID非マッチ（DOS open()準拠）
- **B6**: `fat_file_t`ストリーミングAPI（open/seek/read/tell/size/close）。seekはO(チェーン)再ウォーク、readは`fat_read_file`と同一ガード（bad/free/reserved/EOC短絡/bounds/Brent）を呼び出し内継続、エラー時カーソル不変
- **B7**: `fat_dir_t`カーソルAPI。`fat_iter_dir`は`fat_dir_open`+nextループのthin実装に統合（走査単一化）。END_OF_DIRはsticky。**非ルートディレクトリクラスタは先頭"."エントリ必須の検証**を共有パスに追加（ファイルデータ誤指定をINVALID_ARGで拒否、FAT32のroot_cluster直指定も免除対象）
- **B8**: `fat_dos_date_to_tm`（範囲検証: 月1-12/日≥1/時≤23/分≤59/秒フィールド≤29/tenth≤199）
- **A4**: FAT12ブートセクタ全bit反転4096変異（ASan込み3.8秒）+ FAT16/32値変異（9/13オフセット×3値）— クラッシュ/ASan報告ゼロ
- **A5**: mtools差分（`mdir -b`列挙集合比較×6 Listing、`mtype`バイト一致×3）— 現行libで一致、回帰ロック化
- **A9**: `-Wstrict-prototypes`対応5箇所（color×2、fat_dump×3）、`fat_print_directory_entry`デッドexportと到達不能静的群（約90行）削除、const外し解消

検証（リード直接実施）: クリーンビルド `make check` = **37テスト×2全ok、警告ゼロ**（`-Wall -Wextra -Wshadow -Wstrict-prototypes`、通常+ASan/UBSan）。デモexit=0・全セクション正常。実行時間はASan込み全体で約4秒（ファズフルスイープ含む）。

テスト構成: FAT12基盤9 + ファズ/mtools回帰 + FAT16 5 + FAT32 8 + 新API群（file 6/dir 5/dos日時/0x05/ラベル/ルートdots）= 37。

次のステップ（§12設計済み）: フェーズ4書き込み対応（`fat_set_fat_entry`/`fat_alloc_cluster`/`fat_free_chain`/`fat_add_dirent`/`fat_write_file`/`fat_write`、FAT12 RMW・両FATコピー・FSInfo更新・DOSタイムスタンプ・失敗時一貫性順序）。

## 15. フェーズ4実施結果（2026-10-06 完了）

TDD実施: リードが`fat.h`に書き込みAPI契約を先行確定（§12.8から`fat_write_file`の`tmpl`引数と`FAT_ERR_EXISTS`を追加）→ **p4-test**が全テスト先行記述しred確認（既存シンボルはアサーションred、新シンボル6個はリンクred+`/tmp`参照スタブハーネスでテスト自己整合性を立証）→ **p4-lib**がgreen化（テスト凍結・契約変更なし）。中断なし、両エージェント完走。

達成（`fat_core.c`書き込みセクション約540行 + `fat_dev.c`の`fat_write`）:
- **`fat_set_fat_entry`**: FAT12はread-modify-write（オフセット`cluster*3/2`、偶数/奇数で隣接ニブル保護）。ミラーは全FATコピーへ、extFlags bit7で無効時はactive FATのみ（テーブルkの基底は`reserved_sectors + k*fat_sectors`から導出）。FAT32は上位4予約ビットを既存値から保存。値上限はタイプ別（0xFFF/0xFFFF/0x0FFFFFFF）。
- **`fat_alloc_cluster`**: FAT32はFSInfoのnext_freeヒントを起点に环形走査（staleは2へフォールバック）。EOCマーク+FSInfo同期。空きなしは`DISK_FULL`（走査は読み取りのみでイメージ不変）。
- **`fat_free_chain`**: 歩いたエントリをその場でゼロ化（zero-as-you-goでサイクルも自然破壊）、broken/loop時は歩いた分だけ解放済みの状態で`BAD_CLUSTER`。
- **`fat_add_dirent`**: ワンパス走査（名前衝突/0xE5/0x00/チェーン尾部を1回で収集）。スロット優先順位は**0xE5→0x00→チェーン延長**。FAT12/16固定ルートは`DIR_FULL`。延長は1クラスタゼロ埋め（0x00終端を保証）。
- **`fat_write_file`**: §12.7のクラッシュ一貫性順序（alloc→データ→dirent最後）、失敗時`fat_free_chain`で完全巻き戻し。EXISTS判定は確保前に実施（失敗書き込みが空きクラスタ数を動かさない）。空ファイルはfirst_cluster 0。
- **`fat_write`**: イメージ全体を`path`へflush（作成/切捨て）。

テスト（54テスト、+1004行）: ニブル隣接保護（偶数/奇数クラスタ書き込み後の隣接エントリ不変）、両ミラーraw一致、FAT32予約ビット保存（0xA1234567）、FSInfo増減、0xE5スロット再利用（バイトオフセット完全一致）、EXISTS-before-alloc（空き数監視）、`DIR_FULL`（rootEntryCount=5変異）、FAT32ルート延長（49エントリ、FSInfo -1）、クラスタ境界1023/1024/1025/2048/2049→1/1/2/2/3クラスタ、ロールバック（空き2+5クラスタ要求→DISK_FULLで完全復元）、round-trip+mtoolsオラクル（`mtype`バイト一致・`mdir`列挙一致）、`fat_write`でflush→再読込。

検証（リード直接実施）: クリーンビルド `make check` = **54テスト×2全ok、警告ゼロ**（`-Wall -Wextra -Wshadow -Wstrict-prototypes`、通常+ASan/UBSan）。デモexit=0。変更ファイルは`fat.h`/`fat_core.c`/`fat_dev.c`/`testmain.c`の4つ。

設計からの修正・逸脱:
- **§12.1誤り（正誤）**: 奇数クラスタRMWの設計式`b[1]=value>>8`は誤り。正しくは`b[0]=(b[0]&0x0F)|(value<<4)`、`b[1]=(value>>4)&0xFF`。さらにp4-libが自己発見した`(cluster/2)*3`は奇数で`floor(cluster*3/2)`と一致しない（隣接ニブル破壊）→ `cluster*3/2`（size_t）に修正。
- **§12.8からの逸脱（3件、全てリード承認済み）**: (1) `fat_write`はpath必須（NULL=open元パスの暗黙状態は廃止） (2) `fat_write_file`に`tmpl`引数追加（時刻ホックより決論的、NULL=ATTR_ARCHIVE+ゼロタイムスタンプ） (3) 新enum `FAT_ERR_EXISTS`（無言上書きより明示拒否）。
- **p4-libが自己修正したバグ2件**: FAT12奇数オフセット（上記）、スロット優先順位（初期実装は0x00優先だったが契約どおり0xE5優先に修正）。
- **リード指示の誤りをテスト側が修正**: DIR_FULLのrootEntryCount変異は6でなく5（5生存エントリ+1空きでは追加成功してしまう）。FAT32ルート延長のFSInfo減算は-6でなく-1（`fat_add_dirent`はデータクラスタを確保しない）。

残課題: overwrite/truncate（既存ファイルの再書き込み）、削除（0xE5マーク+チェーン解放）、`fat_file_t`書き込み側カーソル — いずれも本フェーズのスコープ外（契約明記済み）。

## 16. フェーズ5（I/O抽象化）詳細設計（2026-10-06 追記）

目標: 「イメージ丸ごとRAM載せ」を廃止し、`fat_io_t`バックエンド+セクタキャッシュ経由に全領域アクセスを置換。RAM載せは`fat_io_mem`の一実装に。ファイル/ブロックデバイス/FUSE等の並列バックエンドを可能にする。公開契約は`fat.h`のI/O abstractionセクションに確定済み（`fat_io_t` vtable、`fat_io_mem`/`fat_io_file`/`fat_open_io`/`fat_sync`）。

### 16.1 バックエンド契約（fat.h）

- vtable構造体は公開・ユーザー埋め込み可能（独自構造体の**先頭メンバ**として埋め、ダウンキャストで回収）。
- read/write/size/close。size()は不変。範囲外転送は`FAT_ERR_IO`、長さ0は`FAT_OK`。closeはNULL可、呼ばれるのはctxクローズ時ちょうど1回。
- 所有権: `fat_open_io`成功時ctxがioを所有（`fat_close`がflush+close）。失敗時は呼び出し側維持。
- `fat_io_file`: fopen "r+b"、失敗時"rb"フォールバック（読み取り専用ファイルでも開ける。書き込みはflush時に`FAT_ERR_IO`）。

### 16.2 セクタキャッシュ仕様（fat_core.c内部）

- **direct-mapped 16スロット**、スロット=1セクタ（BPB確定後に`bytes_per_sector`分を確保）。インデックス=`セクタ番号 % 16`。
- スロット: {valid, dirty, セクタ番号, データ}。読み: ヒット→コピー、ミス→追い出し（dirtyなら`io->write`でフラッシュ）→`io->read`で充填。書き: 対象セクタを充填（ミスなら読み）→バイト修正→dirtyマーク（**write-back**）。
- `fat_sync(ctx)`: 全dirtyスロットをフラッシュ。`fat_close`: best-effortでフラッシュ→`io->close`→キャッシュ/ctx解放。フラッシュ失敗は`fat_sync`でのみ観測可能（closeはvoid）。
- 教育目的の設計判断: LRUでなくdirect-mapped（O(1)・説明容易）。ミラーFATテーブル間の衝突により性能低下はあり得るが正しくはない。
- **sync前の早期フラッシュ**: スロット衝突による追い出しでdirtyセクタは`fat_sync`以前にフラッシュされ得る。よって「sync前にバックエンド不変」は一般には保証されない（fat.h文面を「no later than fat_sync()/fat_close()」に修正済み）。

### 16.3 内部API（fat_internal.h、p5-core所有）

```c
// キャッシュ経由の読み書き。任意スパン対応（セクタ境界を跨ぐFAT12
// エントリを含む）、[0, ctx->image_size)で境界検査、長さ0はFAT_OK。
fat_result_t fat_io_read(fat_ctx_t* ctx, uint64_t offset, void* buf, size_t len);
fat_result_t fat_io_write(fat_ctx_t* ctx, uint64_t offset, const void* buf, size_t len);
```

- `struct fat_ctx`から`uint8_t* image`を削除し、`fat_io_t* io`+キャッシュ配列+`image_size`（=open時の`io->size()`スナップショット）へ。
- BPB検証はctx構築前のため`io->read`直接（512バイト）。`fat_ctx_init_mem`は`fat_ctx_init(fat_io_t* io, fat_ctx_t** out)`へ置換（失敗時ioを閉じない）。
- **ポインタ保持の全面禁止**: `fat_region_ptr`/`fat_fat_ptr`/`cluster_ptr`/`cluster_mut_ptr`/`fat_mut_ptr`は廃止。全アクセスを`fat_io_read`/`fat_io_write`のオフセット演算に置換。カーソル（`fat_dir_t`/`fat_file_t`）は生ポインタを保持せず、`fat_dir_next`は32バイトを直接`d->raw`へio_read（キャッシュで安価）。`fat_read_file`/`fat_file_read`はユーザーバッファへ直接io_read。

### 16.4 既存APIの再実装

| API | 新実装 |
|---|---|
| `fat_open(path)` | `fat_io_file`構築+`fat_open_io`（全読み込み廃止） |
| `fat_open_mem` | `fat_io_mem`（呼び出し側バッファの私的コピー）+`fat_open_io` |
| `fat_write(ctx, path)` | 論理イメージ全体のエクスポート（キャッシュ経由読みでチャンク単位にfwrite）。バックエンドへのフラッシュではない |
| 書き込みAPI群 | 全操作がキャッシュ経由に。§12.7の失敗時一貫性順序はキャッシュのRAM整合性で保存 |

### 16.5 意味論の変更（文書化済み）

- `fat_open`で開いたファイルへの書き込みは、`fat_sync`/`fat_close`で**ファイル本体に到達する**（旧: RAMコピーのみ、`fat_write`で明示エクスポート）。
- **フィクスチャ保護（重要）**: `demof12/16/32.fat`を`fat_open`+書き込み+closeで変更してはならない。書き込み系テストは`fat_open_mem`かカスタムバックエンドか/tmpコピーで行う。デモ（main.c）は読み取りのみのため無変更・無影響。

### 16.6 エラー伝播

- バックエンドread/write失敗は起動元APIから`FAT_ERR_IO`で伝播。BPB読み取り失敗は`FAT_ERR_IO`、内容不正は従来どおり`FAT_ERR_INVALID_BPB`。

### 16.7 テスト計画（p5-test）

- カスタム計数バックエンド（構造体埋め込みイディオムの動作保証）: read/write呼び出しパターン・呼び出しオフセット・境界外`FAT_ERR_IO`・close厳密1回。
- write-back可視性: 書き込み直後はバックエンドバッファ不変、`fat_sync`後に変化が一致、closeでもフラッシュ。
- read fail注入: N回目のread失敗→起動APIが`FAT_ERR_IO`。
- FAT12セクタ跨ぎエントリ: クラスタ341（バイトオフセット511、`341*3/2=511`）のRMWがセクタ境界を正しく跨ぐことと隣接エントリ340/342不変。
- `fat_io_file`実ファイルround-trip（/tmpコピーで）: 書き込み→sync→再open。
- 既存54テスト: **アサーション凍結**。内部構造消滅に伴う機械的適応（例: `ctx->image`直接参照の除去）のみ許可、全件レポートで列挙。
- 新シンボルはリンクred+/tmp参照スタブハーネスで自己整合性立証（前回と同じ手順）。

### 16.8 分担

- **p5-test**: testmain.c/Makefile（red確定まで。libファイル・fat.h編集禁止）。
- **p5-core**: fat_internal.h/fat_core.c（キャッシュ、`fat_io_read/write`、全ポインタアクセス置換、`fat_open_io`/`fat_sync`、ctx再構成）。
- **p5-dev**: fat_dev.c（`fat_io_mem`/`fat_io_file`、`fat_open`/`fat_open_mem`の薄ラッパー化、`fat_write`エクスポート）。
- **p5-dump**: fat_dump.c（ポインタアクセス排除、`fat_io_read`/`fat_get_fat_entry`経由へ）。
- main.c（デモ）は読み取りのみのため変更不要。リードが統合検証（`make check`×2+デモ、§17記録）。

### 16.9 RED確定（2026-10-06、p5-test完了）

- テスト66（新規12、旧54は**機械的適応ゼロ**・アサーション凍結）。リンクred=undefined 4シンボル（`fat_io_mem`/`fat_io_file`/`fat_open_io`/`fat_sync`）、アサーションred 1（`test_open_write_reaches_file`: 現行libはclose後もファイル不変）、lib非依存2はstubで立証。`/tmp/p5stub`ハーネスでテスト自己整合性と旧54非侵害を立証（残57 green実測）。
- フィクスチャ保護実測: FNV-1aガードテスト（スイート先頭スナップショット→末尾再検証）、全実行後`git status`で0件変更。
- 確定した契約論点（リード裁決）:
  1. **eviction早期フラッシュを許容**。writeback可視性テストは「非衝突セクタのsync前不変」（`fat_set_fat_entry`のみ使用）と「sync/close後の最終状態」に分離。
  2. **openは`io->read`ちょうど1回**（512バイトBOOTセクタ）。<512Bバックエンドは事前sizeチェックで`INVALID_BPB`にせず、read失敗の伝播で`FAT_ERR_IO`。
  3. 失敗時の`*out`値は未規定のまま（テストはNULL初期化のみ）。`fat_sync(NULL)`は`FAT_ERR_INVALID_ARG`に規定（doc追記）。
  4. `fat_io_file`の"r+b→rb"フォールバックは実行環境がrootで検証不可。書き込み失敗伝播はfail-injectバックエンドで一般契約として検証済み。
- スコープ追加（ユーザー指示）: §10 noteの**dump色サイクルstatic解消**（`fat_dump.c:9`の`static int fat_print_color`）をp5-dumpに割当。

## 17. フェーズ5実施結果（2026-10-06 完了）

TDD実施: リードが`fat.h`のI/O abstraction契約を先行確定 → **p5-test**が66テストを先行記述しred確定（§16.9）→ **p5-core**（キャッシュ/`fat_io_read`/`fat_io_write`/ctx再構成/全ポインタアクセス置換）・**p5-dev**（`fat_io_mem`/`fat_io_file`/`fat_open`/`fat_open_mem`の薄ラッパー化/`fat_write`エクスポート）・**p5-dump**（`fat_io_read`/`fat_get_fat_entry`経由化+色サイクルstatic解消）が並列green化 → リードが統合検証。

達成:
- **`fat_io_t`バックエンド**: vtable公開・ユーザー構造体先頭埋め込み（ダウンキャスト回収）。`fat_io_mem`（私的コピー）・`fat_io_file`（"r+b"→"rb"フォールバック）・`fat_open_io`（openは512Bブートセクタ読み込みちょうど1回、vtable NULL検査、成功でctxがio所有・失敗で呼び出し側維持）。
- **セクタキャッシュ**: direct-mapped 16スロット（インデックス=`sector%16`）、write-back、dirty追い出し時の早期フラッシュ許容（fat.h文面「no later than fat_sync()/fat_close()」に整合）。フラッシュ失敗時スロットはdirtyのまま（`fat_sync`が再試行）。充填失敗時キャッシュ状態不変。
- **`fat_io_read`/`fat_io_write`**: 任意スパン（FAT12セクタ跨ぎ対応）、`[0, image_size)`境界検査、長さ0は`FAT_OK`。`struct fat_ctx`から`uint8_t* image`を削除し`image_size`スナップショットへ。`fat_region_ptr`等ポインタ保持アクセサは全面廃止（残存ゼロをgrep確認）。キャッシュ外の直接`io->`呼び出しはflush/fill/init/closeの4箇所のみ。
- **FAT12セクタ跨ぎ実証**: クラスタ341（バイトオフセット511）のRMWが340/342を破壊しないことをテスト確認。
- **dump色サイクルstatic解消**: `fat_print_color`廃止、色カウンタは呼び出しローカル（`int* color`引数）。出力同一性は「旧staticは常に使用前にCL_REDリセット」により保存（§10 note閉鎖）。

テスト（66テスト、+679行、新規12・旧54は適応ゼロ）: 計数バックエンド（read/writeパターン・境界外`FAT_ERR_IO`・close厳密1回）、writeback可視性（sync前不変=非衝突セクタ/`fat_set_fat_entry`のみ、sync/close後最終状態）、read/write失敗注入→`FAT_ERR_IO`伝播、`fat_io_file`実ファイルround-trip、`fat_open`書き込みがファイル本体に到達、FNV-1aフィクスチャガード（スイート先頭スナップショット→末尾再検証）。

検証（リード直接実施）: クリーンビルド `make check` = **66テスト×2全ok（132）、警告ゼロ**（`-Wall -Wextra -Wshadow -Wstrict-prototypes`、通常+ASan/UBSan）。デモexit=0。変更7ファイル（+1639/-368）。`git status`でフィクスチャ0件変更。

設計からの修正・逸脱（リード裁決、§16.9記載の再録）:
1. fat.hの「sync時にのみ書き戻し」文面は§16.2と矛盾 → **早期フラッシュ許容**に修正（テストは非衝突セクタの不変性に分離）。
2. `fat_open_mem`はsize<512を事前検査して`INVALID_BPB`（旧契約互換）。`fat_open_io`はread失敗伝播の`FAT_ERR_IO`（事前sizeチェックなし）— 同じ100B入力でもAPIで挙動が分かれる。
3. open時の`io->read`はブートセクタ512Bちょうど1回（<512Bバックエンドは`FAT_ERR_IO`）。
4. 失敗時の`*out`は未規定。`fat_sync(NULL)`は`FAT_ERR_INVALID_ARG`。
5. "r+b→rb"フォールバックは実行環境がrootで検証不可（書き込み失敗伝播はfail-injectで一般契約検証済み）。
6. **dumpのパディング出力バグ修正**（M2級の表示バグ）: FAT12ダンプが末尾`000 `パディングを行単位に出力していた（103行→72行、実エントリはバイト同一）。旧テストは影響ゼロ（dumpはスナップショット非検証だったため）。

決定事項:
- **フィクスチャ戦略（§5未決定の決着）**: 凍結+ハッシュガード。`demof*.fat`はgit追跡の実物のまま、テストスイートがFNV-1aでスナップショット→再検証、CI相当の`git status`検証はリード実行。mtools再生成によるOEM差異問題はOEMアサート削除済みで非再発。
- §0残課題の`tags`git追跡は既に解除済み（本フェーズ時点で`git ls-files`非対象）。

残課題（フェーズ6候補）: §15残件のoverwrite/truncate・削除（0xE5+チェーン解放）・書き込み側ファイルカーソル。フェーズ3 LFNは引き続きユーザー見送り。

## 18. フェーズ6（書き込み追加: 削除・上書き・truncate）詳細設計（2026-10-06 追記）

目標: §15残課題3件を実装。公開契約は`fat.h`に確定済み（`fat_unlink`/`fat_rmdir`/`fat_file_open_write`/`fat_file_truncate`/`fat_file_write`、新enum `FAT_ERR_DIR_NOT_EMPTY`）。LFNは引き続き見送り。

### 18.1 削除 `fat_unlink` / `fat_rmdir`

- **`fat_unlink(ctx, dir_cluster, name)`**: lookup（NOT_FOUND/ラベル非マッチは既存規則）→ 対象が通常ファイルであること（ATTR_DIRECTORYは`INVALID_ARG`）→ ATTR_READ_ONLYは`INVALID_ARG`（DOSのaccess denied相当）。**整合順序: dirent先頭バイトを0xE5にしてから`fat_free_chain`**（中断時に「解放済みクラスタを指す生存エントリ」を作らない。リークは許容・fsck回復可能）。
- **`fat_rmdir(ctx, dir_cluster, name)`**: ATTR_DIRECTORY必須（ファイルは`INVALID_ARG`）、root自身は`INVALID_ARG`、空であること=「"."/".."以外の生存エントリなし」（0xE5スロットと0x00終端は阻害しない）でないと`FAT_ERR_DIR_NOT_EMPTY`。削除順序はunlinkと同じ（スロット0xE5→チェーン解放）。
- スロット特定: `dir_scan`と同じ走査でname11一致+first_cluster一致の生存スロットのバイトオフセットを特定。

### 18.2 書き込みカーソル

- **`fat_file_open_write(ctx, dir_cluster, name)`**: 既存通常ファイルのみ（NOT_FOUND作成なし、ディレクトリ/ラベル/READ_ONLYは`INVALID_ARG`）。読み取りカーソル（`fat_file_open`）との相違点は**direntスロット（バイトオフセット）を保持**し、truncate/writeがfirst_cluster・file_sizeをスロットへ書き戻す点。tell/seek/readは読み取りカーソルと同一規則（sizeはtruncate/write後の値を反映）。
- **`fat_file_truncate(f, size)`**:
  - 縮小: **dirent sizeを先に更新**→余剰クラスタ解放（部分クラスタの残バイトは不清掃、file_sizeがreaderを拘束）。位置は`min(pos, size)`にクランプ。
  - 拡大: ゼロ埋め（元末尾の部分クラスタ+新規クラスタ）→**データ後にdirent size更新**。
  - `truncate(0)`: チェーン全解放+first_clusterを0に戻す（`fat_write_file`空ファイルと対称）。
  - タイムスタンプは更新しない（決定論性。phase 4のtmpl方針と整合）。
- **`fat_file_write(f, buf, len, written)`**: カーソル位置に書き込み、EOF超過時はクラスタ確保して拡張。ショートなし（FAT_OKなら`*written == len`）。失敗時は**一貫したプレフィックス**（書き込めた分のデータ+dirent size）を残してエラー。seekはsize内に限定されるためホールは発生しない。

### 18.3 整合順序の設計根拠（§12.7の継承）

- 削除: dirent無効化→チェーン解放（逆向きは解放済みクラスタへの生存参照を作り得る）。
- 縮小: dirent size→チェーン解放（逆だとsizeが大きいままチェーン短縮=BAD_CLUSTER参照）。
- 拡大: データ・チェーン→dirent size（逆だとsizeが到達不能データを主張）。
- いずれもキャッシュの早期フラッシュ（§16.2）によりディスク上の順序は保証されない点はphase 5と同一前提。

### 18.4 テスト計画（p6-test）

- unlink: スロット0xE5（バイトオフセット検証）、FAT鎖解放・FSInfo増加、空ファイル（cluster 0）、NOT_FOUND、ディレクトリ対象`INVALID_ARG`、READ_ONLY`INVALID_ARG`、周辺エントリ無傷、round-trip再open。
- rmdir: 空dir削除（dirent+FAT）、`DIR_NOT_EMPTY`、ファイル対象`INVALID_ARG`、root`INVALID_ARG`、0xE5スロットのみのdirは削除可。
- truncate: 縮小（size・tailクラスタ解放・FAT）、拡大ゼロ埋め（読み戻し0）、`truncate(0)`→first_cluster 0、クランプ、空ファイルからの拡大。
- write: 同サイズ上書き（内容置換・size不変）、EOF越え拡張（size・チェーン・FSInfo減）、seek中間書き込み、同一カーソルでのread-back、fail-injectバックエンドでのエラー伝播とプレフィックス一貫性、`open_write`のNOT_FOUND/dir/READ_ONLY。
- オラクル: mtools（`mdel`/`mcopy -o`）との事後状態比較、mtype内容一致。
- 既存66テストはアサーション凍結。新シンボル5個はリンクred+/tmpスタブハーネス。フィクスチャ保護（memバックエンド/FNV-1aガード）継続。

### 18.5 分担

- **p6-test**: testmain.c/Makefile（red確定まで。libファイル・fat.h編集禁止）。
- **p6-lib**: fat_internal.h/fat_core.c（`fat_strerror`への`DIR_NOT_EMPTY`追加含む）。fat_dev.c/fat_dump.cは変更不要。
- リードが統合検証（`make check`×2+デモ+フィクスチャ不変+`git status`）、§19記録、コミット。

## 19. フェーズ6実施結果（2026-10-06 完了）

実施形態: 前セッションで契約+RED確定済み（`ef19f2b`）のため、本セッションは**リードがp6-lib緑化を直接実施**（fat_core.cのみ編集、fat.h/fat_internal.h/fat_dev.c/fat_dump.cは変更ゼロ）し、統合検証まで完遂。実行環境はLinux（Ubuntu、clang、mtools 4.0.43）に移行したため、macOS前提だったビルドのポータビリティ修正を含む。

達成（§18.1-18.3実装、fat_core.c +504行）:
- **削除**: `fat_unlink`/`fat_rmdir`。スロット特定は`dir_scan`流用（`DirScan`に`match_offset`を追加、ボリュームラベルは非マッチ=lookup統一）。整合順序はdirent先頭バイト0xE5→`fat_free_chain`（`dir_slot_delete`共通化）。rmdirは`fat_iter_dir`で空判定（"."/".."は`fat_dot11`/`fat_dotdot11`のraw11バイト比較で除外、0xE5/0x00終端/LFNはイテレータが既にスキップ）、ファイルは`INVALID_ARG`、root（cluster 0 / `geo.root_cluster`）は`INVALID_ARG`。READ_ONLYは両APIとも`INVALID_ARG`。
- **書き込みカーソル**: `fat_file_open_write`（direntスロットのバイトオフセットを`slot_offset`として保持、READ_ONLY/dir/label拒否）→ `fat_file_truncate`（縮小=dirent size→cut点EOC→`fat_free_chain`、`truncate(0)`=dirent(cluster 0,size 0)→全解放、拡大=部分クラスタ末尾ゼロ化→新クラスタ割当・リンク→dirent size。失敗時は追加クラスタを巻き戻し、dirent size不変）→ `fat_file_write`（上書き/追記を単一ループで処理、クラスタ境界でFAT追従、EOF+境界で`fat_alloc_cluster`、**成功/失敗ともにコミット時にdirentへプレフィックス整合を書き戻し**）。`dirent_slot_patch`は32バイトRMWでfirst-cluster/fileSizeのみ更新（タイスタンプ他はバイト不変=§18.4のスロット整合テストが直接検証）。`fat_strerror`に`DIR_NOT_EMPTY`追加。
- **EOF-entry書き込みの実装上の要点**: `fat_file_seek`は`offset == size`で再ウォークしないため、書き込み開始時に`pos == size`なら`chain_step`でチェーン最終クラスタを特定し直す（読み取りカーソルの契約「clusterはpos<sizeの間のみ有効」の厳守）。

テスト修正（RED凍結からの逸脱、いずれも**グリーン実行が一度もされていないため潜伏したテスト側バグ**。リード裁決で修正）:
1. `test_rmdir_deleted_slots12`: `fat_unlink(ctx, 11, ...)` → `fat_unlink(ctx, d2s1, ...)`。page.txt/test_5kb.txtはdir2/subdir1（d2s1=12）にあり、dir2（11）直下には存在しない（テスト自身のコメントの意図どおり）。11のままだと`NOT_FOUND`で原理的にグリーン化不能。
2. `assert_slot_only_size_cluster_changed`: first-clusterの組立が`(high) | (low << 16)`と高低逆。実フィクスチャでは常に失敗するヘルパーバグ → `(high << 16) | low`に修正。
3. `test_rmdir_fat32`: 「sub1（cluster 57）は空」という前提が誤り（フェーズ2の`test_fat32_lookup_read`が`dir1/sub1/page.txt`（cluster 58）の存在をピンしているため両立不可能）→ `fat_unlink(ctx, 57, "page.txt")`で空にしてからrmdirする形に再構成、FSInfo期待値を+1/+2に更新。意図（FAT32でのクラスタ解放+FSInfo整合）は保存。
4. `Makefile`: `-D_POSIX_C_SOURCE=200809L`をCFLAGSに追加。glibcは`-std=c99`で`popen/pclose`を非公開にする（macOSのlibcは非条件付で公開）。mtoolsオラクルヘルパーは旧来からpopenを使用しており、Linuxビルドでは旧フェーズから失敗する問題だった。

検証（リード直接実施）: クリーンビルド `make check` = **91テスト×2全ok（182）、警告ゼロ**（`-Wall -Wextra -Wshadow -Wstrict-prototypes`、通常+ASan/UBSan）。デモexit=0（cat出力正常）。`demof12.fat`はコミット済みフィクスチャ（mtools 4.0.48製、md5 f9d775d1…）を復元のうえ再検証し、スイート内FNV-1aガードも通過。`demof16/demof32.fat`はgitignoreのローカル生成物としてmtools 4.0.43（Ubuntu）で再生成（ハードコードされたクラスタ番号14/15/57/58・フリー数66454等はすべて一致してグリーン=フィクスチャレシピの再現性を確認）。変更3ファイル（+519/-9）。`fat.h`・`fat_internal.h`・`fat_dev.c`・`fat_dump.c`は変更ゼロ。

決定事項:
- `dir_scan`のマッチ対象からボリュームラベルを除外（lookup/open_write/unlink/rmdirで一貫）。副効用として`fat_add_dirent`のEXISTS判定もラベル非マッチに寄る（fat.hの「a live entry with this name」解釈をlookup規則に統一。既存テストは影響なし=確認済み）。
- `demof16/demof32.fat`は各マシンで`make fat16`/`make fat32`により再生成する運用を継続（凍結対象はコミット済みの`demof12.fat`のみ）。

残課題: フェーズ3 LFNはユーザー見送り継続。デモ（`main.c`）はFAT12フィクスチャのみ。

追記（同日、§17.5の完遂）: "r+b→rb"フォールバックを実機検証した（非root・uid 1000）。読み取り専用コピー（chmod 444）に対し`fat_open`が成功（`fopen("r+b")`失敗→`"rb"`フォールバック）、読み取りはバイト一致、キャッシュ書き込み（unlink）は成功、`fat_sync`は`FAT_ERR_IO`、ディスク上のファイルはmd5一致でバイト不変。対照として書き込み可能コピーは`fat_sync`=`FAT_OK`。ASan/UBSan付きで実施（プローブ: `/tmp/p6dbg/p6dbg6.c`）。§17.5は本検証をもって閉鎖。

## 20. フェーズ7（LFN完全対応）詳細設計（2026-10-06 追記）

目標: フェーズ3として見送っていたLFN（Long File Name）を読み書き両面で完全対応する。公開契約の変更は`fat.h`の文面のみ（新API・新enumなし。`fat_dirent_t.name[FAT_NAME_MAX=256]`は当初からLFN前提サイズ）。運用はフェーズ6と同じ: リードが§20設計+契約を確定 → p7-test がRED確定（テスト凍結）→ p7-lib が緑化 → リード検証。

### 20.1 オンディスクLFN仕様（実装対象）

- LFNエントリは属性バイト[11]=0x0F（ATTR_LONG_NAME）。32バイトのうち名前領域は UTF-16LE で3箇所: [1..10]=name1[5]、[14..25]=name2[6]、[28..31]=name3[2]（計13文字/エントリ）。byte0=シーケンス番号（1..20）、論理的に最後のエントリ（=物理的に**最初**）のみ0x40フラグ（0x41..0x5F…最大`0x40|20`）。byte12=型（0）、byte26=firstClusterLow（0）。
- 物理順序は**逆順**: seq N（最大、0x40付き）が先頭、seq 1が8.3エントリ直前。最大20エントリ=255 UTF-16文字（260文字中5文字は8.3側とNUL終端の冗長分…正確にはLFN最大255文字+NUL）。
- 名前の終端は0x0000、以降の同名領域は0xFFFFパディング。
- チェックサム（byte13）: 対応する8.3名11バイトに対し`sum = ((sum & 1) << 7) + (sum >> 1) + name[i]`（mod 256、初期0）。

### 20.2 読み取り（結合・検証・フォールバック）

- 統合ポイントは`fat_dir_next`（fixed/chain両モード）1箇所のみ（`fat_iter_dir`はcursor thin実装、`dir_scan`は12バイトhead比較でLFN非対象）。
- 走査中のATTR_LONG_NAMEエントリはraw32を一時バッファ（最大20個）に蓄積。8.3エントリに到達したとき:
  1. シーケンス検証: 蓄積数N、物理先頭が`0x40|N`、以降N-1..1の降順であること。
  2. チェックサム検証: 全LFNエントリのbyte13が8.3名11バイトのチェックサムに一致すること。
  3. 復号: UTF-16LE→UTF-8（サロゲートペア対応。0x0000=終端、0xFFFF=パディング）。変換結果（NUL終端込み）が`FAT_NAME_MAX`（256）バイトに収まること。
  - **全検証パス→`entry->name`にLFN**（属性・クラスタ・サイズ等は8.3エントリから、现行どおり）。**いずれか失敗→8.3名フォールバック**（オーフォン: チェックサム不一致・seq不連続・8.3 follower不在・0xE5/0x00による分断、すべて同様）。
- 0xE5・0x00エントリで蓄積はリセット（削除済みLFN系列は無視）。蓄積上限20超（不正ディレクトリ）はフォールバック+蓄積リセット。
- `raw32`コールバックには8.3エントリの32バイトを渡す（dumpビューの互換維持。LFNエントリ自体はコールバックに流れない=現行挙動と同一）。

### 20.3 名前マッチング（lookup/unlink/rmdir/open_write/add_direntのEXISTS判定）

- `fat_dirent_t.name`（LFN優先・8.3フォールバック）との比較に統一。照合は**ASCII範囲で大文字小文字無視**（8.3側が従来`fat_name_to_83`のto-upperでcase-insensitiveだったことと一貫。UTF-8マルチバイト部はバイト完全一致）。
- `fat_lookup`のパス解決は各コンポーネントをこの規則で照合。"."/".."の特別処理は现行どおり。
- 既存の8.3エントリのみのフィクスチャでは`fat_name_to_83`で大文字化した名前と`dirent->name`（=8.3描画・大文字）が一致するため、既存テストの照合結果は不変。

### 20.4 書き込み（fat_write_file / fat_add_dirent）

- 名前分流: `fat_name_to_83`が成功する名前（=8.3で可逆表現可能）は**従来経路のまま8.3のみ**（LFNなし。既存テスト互換）。`NAME_TOO_LONG`となる名前はLFN要件（長さ1..255 UTF-8バイトかつ1..255 UTF-16文字、`'/'` `'\\'`を含まない、NUL不含）を満たせばLFN経路:
  1. **8.3エイリアス生成**（Windows流マングリング）: ベース先頭からprefixを取り、`~N`を付ける。N=1..、prefix長はNの桁に応じ6/5/4/3と縮める。拡張子は最後の`.`以降から3文字（同フィルタ）。文字フィルタは**リード裁定（2026-10-06、p7-test実測に基づく）**: スペースは**除去（詰め）**、無効文字`"*+,;<=>?[\\]|:`・制御文字・非ASCII（0x80以上）は**1文字につき1個の`_`置換**（mtools 4.0.43実測: "a long name file"→`ALONGN~1`、日本語名→`_`並び、Windows実挙動とも一致）。ディレクトリ内既存8.3名（LFNエントリを除く生存エントリ）と衝突しない最小のNを採用。
  2. **スロット確保**: 必要`n_lfn + 1`連続枠。走査は0xE5の**連続run**を追跡（run >= needならrun先頭を採用）、0x00到達時は0x00以降から連続取得（固定ルートは残スロット数検査→`DIR_FULL`、チェーンは0x00ゼロ埋め延長=现行どおり）。need=1のときは现行の「最初の0xE5→0x00→延長」優先順位と完全一致（既存テスト互換）。
  3. **整合順序**: データチェーン→LFNエントリ群（seq降順で物理書き込み）→8.3エントリ（最後）。失敗時の巻き戻しは§12.7どおり（確保チェーン解放。スロットは0x00領域に書いた分はそのまま残り得る=8.3エントリ不存在により全员オーフォン化、無害）。
- UTF-8→UTF-16LE変換（書き込み側）: 1..4バイトUTF-8をデコード、4バイト分はサロゲートペア。255 UTF-16文字上限。終端0x0000+0xFFFFパディング。
- `fat_write_file`のEXISTS判定はLFN名・エイリアス名の両方で照合（§20.3規則）。

### 20.5 削除（fat_unlink / fat_rmdir）

- §18.1の整合順序（スロット0xE5→チェーン解放）をLFN系列に拡張: マッチした8.3スロットに**先行する連続ATTR_LONG_NAMEエントリのうち、チェックサムが一致するもの**を同時に0xE5化（チェックサム不一致のオーフォンは生存放置=§20.2と対称）。書き込み順は「LFN群→8.3スロット→チェーン解放」。
- `fat_file_open_write`/`fat_file_truncate`/`fat_file_write`は`slot_offset`束縛のため変更不要（名前解決のみ§20.3対応）。

### 20.6 実装スコールール

- fat_internal.h: LFNエントリのパック/アンパック・チェックサム・UTF-16↔UTF-8変換の内部関数プロトタイプと`fat_dir`構造体へのLFN蓄積バッファ追加（`fat_core.c`内staticでも可。エージェント裁量）。
- fat_core.c: `fat_dir_next`（両モード）の結合、`lookup_in_dir`/`DirScan`のマッチング拡張（LFN名比較）、`fat_add_dirent`/`fat_write_file`のLFN経路、`dir_slot_delete`のLFN系列化、`fat_lookup`の照替え。stdioゼロ・警告ゼロ維持。
- fat_dev.c / fat_dump.c / Makefile: 変更不要（予定）。

### 20.7 テスト計画（p7-test）

- フィクスチャは凍結のため、LFN対象は`fat_open_mem`イメージへのヘルパー直接書き込み（LFNエントリ+8.3エントリを合成）またはmtoolsオラクル（`mcopy`のLFN生成を`mdir -b`/`mtype`で比較）で用意。
- 読み取り: 合成LFN（1/2/20エントリ、日本語・サロゲートペア含む）のname・属性・クラスタ、チェックサム不一致/seq不連続/0x40欠落/孤立LFN（0x00直前）→8.3フォールバック、0xE5分断、UTF-8 255バイト境界。
- 照合: LFN名lookup成功（case-insensitive）、8.3エイリアス名でも同一エントリ、LFNファイルのunlink/rmdir/open_write/truncate。
- 書き込み: LFNファイル作成→再読取でLFN名・内容一致、mtoolsオラクル（`mdir`のLFN表示・`mtype`内容一致）、エイリアス衝突で~2、ルート満杯`DIR_FULL`（need>残りスロット）、`fat_write_file`のEXISTS（LFN名・エイリアス名両方）。
- 削除: LFNファイルunlink→LFN系列全0xE5（バイトオフセット検証）・チェーン解放、オーフョン（チェックサム不一致LFN）は0xE5化されない。
- 既存91テストはアサーション凍結（LFN結合による`dirent->name`変化はフィクスチャにLFNが存在しないため影響ゼロ）。新規シンボルなし（リンクredは発生しない想定; 全件アサーションred想定）。

### 20.8 分担

- **p7-test**: testmain.c（RED確定まで。libファイル・fat.h編集禁止）。
- **p7-lib**: fat_internal.h / fat_core.c（緑化）。
- リードが統合検証（`make check`×2+デモ+フィクスチャ不変+`git status`）、§21記録、コミット。

## 21. フェーズ7（LFN完全対応）実施結果（2026-10-06）

TDD実施: リードが§20詳細設計+`fat.h`契約コメント確定 → **p7-test**が29テスト（計120）を先行記述しRED確定 → **p7-lib**が`fat_core.c`のみで緑化 → リードが統合検証・コードレビュー。

### 21.1 RED確定時のリード検証（プレグリーン期待値検証）

- 個別ハーネス（assert中断回避のper-test実行、`/tmp/p7red/redcheck.c`）で29テストの失敗理由を1件ずつ確認: **22 RED / 7 GREEN**（GREENは設計どおりのロック: ヘルパーmtools実測ピン止め1・フォールバック規定5・8.3非回帰1）。
- テスト側潜伏バグ1件を発見・裁定修正（凍結逸脱記録付き）: `test_unlink_lfn_orphan_survives` — `put_lfn_run`が常に有効チェックサムを書くため「チェックサム破損orphan」の前提が不成立（実配置 `[slot5=csum有効][slot6=0x42][slot7=8.3(上書き)]`）。slot5のチェックサムを事後破損（`^= 0xFF`）+victim系列をslot6-7+8.3をslot8へ再配置。教訓（p7-testも追試で同意）: 変異注入テストは「ヘルパー出力を破損させる操作」自体を実測裏取りするまでが前提検証。
- エイリアス生成のスペース処理を裁定確定: **スペースは除去（詰め）**、無効文字・制御文字・非ASCIIは1文字につき1個の`_`（mtools 4.0.43実測: "a long name file"→`ALONGN~1`・日本語名→`_`並び、Windows実挙動と一致）。§20.4文面更新済み。
- 既存テスト2件の契約置換に伴う裁定修正（p7-libが実装前調査で発見・報告、リードが修正）: `test_add_dirent_errors`/`test_write_file_small`の`"toolongname.txt"`=NAME_TOO_LONG期待はフェーズ4契約（8.3超過=即エラー）の名残でLFN契約と論理的に両立不可能。エラー経路の検証意図を保持し299文字`'a'`（LFN上限255 UTF-8バイト超過）へ置換。§20.7「既存91テスト影響ゼロ」の見立て漏れだった（旧実装ではgreenのためRED検証で発覚不能）。
- 緑化時のテスト側バグ3件（p7-libが報告、リードが検証のうえ裁定修正、いずれも裁定コメント付き）:
  1. `test_write_file_lfn_alias_vs_existing_83`: 純8.3で作った`longna~1.txt`の`de.name`期待が小文字 — LFN runを持たないエントリは8.3描画（大文字）が契約。`"LONGNA~1.TXT"`へ修正。
  2. `test_unlink_lfn_series12`: 「他は不変」memcmpがブート+FAT両ミラーを含む範囲を比較 — テスト自身が`fat_at(cl)==0`（チェーン解放=FAT書き換え）を要求しており原理的に両立不能。ルートスロット0..4のみの比較へ修正。
  3. `assert_mtype_matches_read`ヘルパ: `popen`コマンドのパスがクオートなし — スペース入りLFN名でsh単語分割によりmtypeが破壊。`"%s"`クオート追加。

### 21.2 実装概要（p7-lib、fat_core.c +728/-191、fat_internal.hは無変更）

- 読み取り: `fat_dir_next`両モードの`dir_slot_feed`に集約 — LFNエントリ（attr 0x0F）を`LfnAcc`に蓄積し、続く8.3で`lfn_join`（降順seq+先頭0x40フラグ+全エントリchecksum一致+UTF-16LE→UTF-8（サロゲートペア対応、`FAT_NAME_MAX`超過はフォールバック））を検証、成立なら`dirent.name`を差し替え。raw32コールバックは常に8.3の32バイト。0xE5/0x00で蓄積リセット、21エントリ超過はpoisoned（malformed耐性）。
- 照合: `name_ci_eq`（ASCII範囲大小無視）によるrendered名比較（LFN優先、エイリアス8.3も照合）を`DirScan`（lookup/unlink/rmdir/open_write/EXISTS共通）に統合。ボリュームラベル非マッチは維持。
- 書き込み: `fat_add_dirent`入口で`'/'` `'\\'`拒否（`fat_name_to_83`がたまたま受理する"bad/name"類を封じる）。8.3可逆名は従来経路のまま。LFN経路は`utf8_to_utf16`（妥当性検査: 過長形式・サロゲート・範囲外拒否）→エイリアス`lfn_make_alias`（Windows式BASE~N、スペース除去・無効/非ASCIIは`_`、prefix 6/5/4/3、`~N`は既存live 8.3名と衝突しない最小N）→n+1連続スロット（0xE5 run追跡→0x00→チェーン延長、need=1で旧挙動と完全一致）→整合順序「LFN run（seq降順物理書き込み）→8.3」。
- 削除: `dir_scan_feed`がマッチ時にチェックサム一致LFN runのオフセット群を記録し、`dir_slot_delete`が「LFN群→8.3スロット→チェーン解放」の順で0xE5化（チェックサム不一致のorphanは生存）。`fat_unlink`/`fat_rmdir`共用。
- 書き込みカーソル: `fat_file_open_write`は`DirScan`のqname照合に切替え、slot束縛（`dirent_slot_patch`）は8.3スロットのまま変更なし。
- 実装バグ1件をp7-libが自己発見・修正: "bad/name.txt"が`fat_name_to_83`で受理される問題（上記入口拒否で解決）。

### 21.3 検証結果（リード）

- `make check`: **120テスト×2（通常+ASan/UBSan）全ok**、コンパイラ警告ゼロ（`-Wall -Wextra -Wshadow -Wstrict-prototypes`）。
- デモ`build/fatdemo` exit 0。`demof12.fat`不変（md5 `f9d775d1…`、`git status`クリーン、テストのFNV-1aガードも通過）。
- 変更ファイル: `fat_core.c`（実装）・`testmain.c`（+29テスト+裁定修正5箇所）・`fat.h`（契約コメント）・`REVIEW.md`（§20-21）。`fat_internal.h`/`fat_dev.c`/`fat_dump.c`/Makefile変更ゼロ。
- リードコードレビュー: 結合検証・照合・スロット確保・整合順序・poisoned運用・サロゲート処理を確認、指摘なし。

### 21.4 残課題

- なし（フェーズ7完了。デモのLFN表示は`fat_iter_dir`経由で自動的にLFN名になる）。

## 22. フェーズ8（デモmain.cのFAT16/32対応）詳細設計（2026-10-06 起草、実装は次フェーズ）

目標: デモ（`main.c`）がFAT12フィクスチャ固定（demof12.fat、dir1/dir2構成の固定パス）から、3タイプのフィクスチャすべてを扱えるように段階的に拡張する。ライブラリAPIは既にFAT12/16/32フル対応済みであり、変更はmain.cとMakefile（デモ実行ターゲット）のみを予定。§19残課題「デモはFAT12フィクスチャのみ」の解消。

### 22.1 現状と制約

- `main.c`は`fat_open("demof12.fat")`固定。ダンプ系（`fat_print_info`/`fat_print_header_dump`/`fat_print_fat`）と`fat_lookup`/`fat_read_file`はタイプ非依存に実装済み（FAT32のEBPBダンプ・ルートチェーン走査も§11で対応済み）。
- セクション構成はFAT12フィクスチャのディレクトリ構造（dir1/dir2/subdir1、特定ファイル名）に固定。
- 3フィクスチャの共通構造: ルートにHELLO.TXT（12B）・TEST_5KB.TXT（4962B）・DIR1、dir1配下にSUB1（FAT16/32）またはSUBDIR1/SUBDIR2（FAT12）・HOGE.TXT、sub1配下にPAGE.TXT（14B）。FAT32のみF00..F39.TXT（40ファイル）とルートチェーン3クラスタ。
- demof16/demof32.fatはgitignore・ローカル生成（`make fat16`/`make fat32`）。不在でもデモはFAT12で動作し続ける必要がある。

### 22.2 フェーズ8a: 引数化と共通セクション化（main.c）

- `main(int argc, char** argv)`: `./fatdemo [image-path]`。引数なし=従来どおりdemof12.fat（後方互換）。
- 共通セクション（全タイプで同じコードパス）: FAT info / BPBダンプ / FATダンプ / ルートiterate（dumpビュー）/ ls / / cat hello.txt / cat test_5kb.txt。
- タイプで分岐が必要な表示は`fat_get_type()`で分岐（既存ダンプ関数は内部でタイプ対応済みのため、追加は軽微）。
- 深いパスのセクション（dir1、dir1/sub1 等）は「存在すれば表示」形式へ: lookup失敗（FAT12にsub1が無い等）はstderr警告ではなくセクションスキップに変更。

### 22.3 フェーズ8b: タイプ固有セクション（main.c）

- FAT12: 従来のdir1/dir2/subdir1セクションを維持（共通化された枠組みの上で）。
- FAT16: dir1/sub1/PAGE.txtの走査・cat。
- FAT32: FSInfo表示（`fat_fsinfo`。free count/next free）、ルートチェーン（2→54→55）のクラスタ単位ダンプ、F-fillers（F00..F39）のls、dir1/sub1/PAGE.TXTのcat。
- 出力の対話的な比較可能性のため、セクション見出しはタイプ名を含める（`*** FAT32: FSInfo ***` 等）。

### 22.4 フェーズ8c: Makefileと実行マトリクス

- `make demo12`/`demo16`/`demo32`（16/32はフィクスチャ不在時は生成を促すメッセージでスキップ）、`make demo-all`=fat16 fat32生成+3タイプ連続実行。
- 各デモのexit=0を検証（リードの統合検証手順に組み込み）。
- `make check`への影響なし（テストハーネスはtestmain.cで独立）。

### 22.5 テスト方針

- main.cはデモであり新規ユニットテストは追加しない（既存120テストがライブラリ側をカバー）。検証は3タイプ×デモexit=0と目視ダンプ（リード実施）。
- スコープ外: デモの書き込みAPI実演（fat_write_file等）は別フェーズ候補とする（フィクスチャ保護のため/tmpコピー運用の設計が必要）。

### 22.6 分担（実装フェーズ時）

- 単独ウェーブ（main.c+Makefileのみ、所有権競合なし）。リードが直接実施するか単一エージェント（p8-app）。
