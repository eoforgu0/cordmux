// SPDX-License-Identifier: Apache-2.0

// fetch.mjs が保存した Discord のバンドル (1 つのファイルが webpack の 1 つのチャンク) から、webpack のモジュールの
// 表 (モジュールの id -> 関数) を TypeScript のパーサーで取り出し、依存箇所の一覧の述語 (PredicateSpec) に一致する
// モジュールの数を数える。src-vencord/tests/discord_contacts.test.ts が scanBuildDir を呼ぶ。

import { readdirSync, readFileSync, statSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import ts from "typescript";

/** 依存箇所の一覧の述語。src-vencord/ledger.ts の PredicateSpec と同じ形にする。 */
export type PredicateSpec =
    | { kind: "props"; props: readonly string[] }
    | { kind: "code"; code: readonly string[] };

export interface ModuleBody {
    /** モジュールがあったチャンクのファイル名 (パスではなく basename)。 */
    file: string;
    /** webpack のモジュール id (数値または文字列のキーをそのままテキストにしたもの)。 */
    id: string;
    /** そのモジュールの関数全体のソースのテキスト (パラメーターを含む)。 */
    text: string;
}

export interface ScanResult {
    /** 述語に一致したモジュールの数 (0 は見つからない、1 は 1 つに決まる、2 以上は曖昧)。 */
    matchCount: number;
    /** 一致したモジュールの "ファイル名#モジュールの id" の一覧。 */
    matchedModules: readonly string[];
}

// モジュールの表とみなすオブジェクトリテラルの、プロパティの数の下限。Discord の遅延読み込みのチャンクは 1 つの
// チャンクに 1 つのモジュールの形が多いので、1 にする。5 にすると、保存したバンドルのファイルの 9 割近くから
// モジュールを取り出せなかった。1 にしても、FUNCTION_VALUE_RATIO が値のほぼ全部が関数であることを求めるので、
// 値が文字列だけの対応表 (アイコンやサウンドのファイル名など) は除かれる。
const MIN_MODULE_MAP_PROPS = 1;
const FUNCTION_VALUE_RATIO = 0.9;

function isFunctionValued(member: ts.ObjectLiteralElementLike): boolean {
    if (ts.isMethodDeclaration(member)) return true;
    if (ts.isPropertyAssignment(member)) {
        const init = member.initializer;
        return ts.isFunctionExpression(init) || ts.isArrowFunction(init);
    }
    return false;
}

function propertyKeyText(
    member: ts.MethodDeclaration | ts.PropertyAssignment,
    sourceFile: ts.SourceFile,
): string {
    const name = member.name;
    if (ts.isIdentifier(name) || ts.isPrivateIdentifier(name)) return name.text;
    if (ts.isStringLiteralLike(name)) return name.text;
    if (ts.isNumericLiteral(name)) return name.text;
    return name.getText(sourceFile);
}

/**
 * 1 つのファイル (1 つのチャンク) のソースから、webpack のモジュールの表を取り出す。
 *
 * webpack のチャンクは「id -> 関数」のオブジェクトリテラルを持つ (`(...).push([[chunkIds],{123(e,t,n){...}, ...}])`
 * の 2 番目の要素か、entry のチャンクではランタイムの中の `f={123(e,t,n){...}, ...}`)。正規表現だけで分けると、
 * 入れ子の括弧やコメントの中の紛らわしい文字列で誤るので、TypeScript のパーサーで構文木を作る。プロパティの
 * 9 割以上 (FUNCTION_VALUE_RATIO) が関数で、プロパティが MIN_MODULE_MAP_PROPS 個以上あるオブジェクトリテラルを
 * モジュールの表とみなす。値が文字列だけの大きなオブジェクト (アイコンやサウンドのファイル名の対応表) は除かれる。
 */
export function extractModules(fileName: string, text: string): ModuleBody[] {
    const sourceFile = ts.createSourceFile(fileName, text, ts.ScriptTarget.ES2022, false);
    const modules: ModuleBody[] = [];
    const baseName = path.basename(fileName);

    function visit(node: ts.Node): void {
        if (ts.isObjectLiteralExpression(node) && node.properties.length >= MIN_MODULE_MAP_PROPS) {
            const total = node.properties.length;
            let funcCount = 0;
            for (const member of node.properties) {
                if (isFunctionValued(member)) funcCount++;
            }
            if (funcCount / total >= FUNCTION_VALUE_RATIO) {
                for (const member of node.properties) {
                    if (!isFunctionValued(member)) continue;
                    const typed = member as ts.MethodDeclaration | ts.PropertyAssignment;
                    modules.push({
                        file: baseName,
                        id: propertyKeyText(typed, sourceFile),
                        text: member.getText(sourceFile),
                    });
                }
                // モジュールの表とみなしたら、その中 (各モジュールの関数の本体) には入らない。入ると、
                // モジュールの本文にある別の「id か名前 -> 関数」の形のオブジェクト (Flux の action の
                // ハンドラーの表など) を別のモジュールとして重ねて数え、1 つの一致が何十にも見える。
                return;
            }
        }
        ts.forEachChild(node, visit);
    }

    visit(sourceFile);
    return modules;
}

const IDENTIFIER_SAFE = /^[A-Za-z_$][\w$]*$/;

function escapeRegExp(s: string): string {
    return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function matchesProps(body: string, props: readonly string[]): boolean {
    return props.every((name) => {
        if (IDENTIFIER_SAFE.test(name)) {
            return new RegExp(`\\b${escapeRegExp(name)}\\b`).test(body);
        }
        // 記号を含み、識別子の形でない名前は、部分文字列の一致で見る。
        return body.includes(name);
    });
}

function matchesCode(body: string, code: readonly string[]): boolean {
    return code.every((s) => body.includes(s));
}

/**
 * モジュールの本文が述語に一致するかを返す。実行した後の exports を見る Vencord の findByProps と findByCode の
 * 近似で、Discord を実行せずに本文のテキストだけを見る。
 * - props: 名前が本文に識別子かプロパティ名として現れるか (\b で区切って見る)
 * - code: 文字列が本文にそのまま含まれるか
 * そのため、本文で名前を呼んでいるだけでも一致し、ほかのモジュールから re-export されて exports に現れる名前は
 * 一致しない。code の述語は、実行時は 1 つの関数の toString() を見るが、ここではモジュールの関数全体を見る。
 * 一致の数が 0 でも、近似のために見つからないことがある。2 以上は曖昧なだけで、別々の実装が複数ある証拠ではない。
 * ちょうど 1 でも、実行時に 1 つに決まるとは限らない。
 */
export function matchesPredicate(body: string, predicate: PredicateSpec): boolean {
    return predicate.kind === "props" ? matchesProps(body, predicate.props) : matchesCode(body, predicate.code);
}

/**
 * 述語の文字列のどれかがファイルのテキストになければ、そのファイルのどのモジュールにも一致しない。\b で区切った
 * 一致は部分文字列の一致より厳しいので、この判定は必要条件にすぎない。構文木を作る前に、これで対象のファイルを
 * 絞る。
 */
function couldPossiblyMatch(rawText: string, predicate: PredicateSpec): boolean {
    const needles = predicate.kind === "props" ? predicate.props : predicate.code;
    return needles.every((s) => rawText.includes(s));
}

function listJsFiles(dir: string): string[] {
    const out: string[] = [];
    for (const name of readdirSync(dir)) {
        if (!name.endsWith(".js")) continue;
        const full = path.join(dir, name);
        if (statSync(full).isFile()) out.push(full);
    }
    return out;
}

/** 依存箇所の一覧の 1 行のうち、scan.ts が使う部分。 */
export interface LedgerRow {
    id: string;
    predicate: PredicateSpec;
}

/**
 * buildDir (1 つの build のチャンクが入ったフォルダー) の全部のファイルを 1 回ずつ読み、依存箇所の一覧の行ごとに
 * 一致したモジュールを数える。行の id をキーにした Map を返す。読めないファイルは飛ばす。数千のファイルを行ごとに
 * 何度も読まないように、ファイルごとに、一致しうる行があるときだけ構文木を作る。
 */
export function scanBuildDir(
    buildDir: string,
    rows: readonly LedgerRow[],
): Map<string, ScanResult> {
    const results = new Map<string, ScanResult>();
    for (const row of rows) results.set(row.id, { matchCount: 0, matchedModules: [] });

    for (const filePath of listJsFiles(buildDir)) {
        let rawText: string;
        try {
            rawText = readFileSync(filePath, "utf8");
        } catch {
            continue;
        }

        const candidateRows = rows.filter((row) => couldPossiblyMatch(rawText, row.predicate));
        if (candidateRows.length === 0) continue;

        const modules = extractModules(filePath, rawText);
        for (const row of candidateRows) {
            const result = results.get(row.id);
            if (!result) continue;
            for (const module of modules) {
                if (matchesPredicate(module.text, row.predicate)) {
                    result.matchCount++;
                    (result.matchedModules as string[]).push(`${module.file}#${module.id}`);
                }
            }
        }
    }

    return results;
}

/** 1 つの述語だけを調べる (コマンドラインから使う)。 */
export function scanOne(buildDir: string, predicate: PredicateSpec): ScanResult {
    const results = scanBuildDir(buildDir, [{ id: "_", predicate }]);
    return results.get("_") ?? { matchCount: 0, matchedModules: [] };
}

/* ===================================================================
   コマンドラインから実行するとき:
     node test/tools/discord-bundle/scan.ts <buildDir> --props name1,name2
     node test/tools/discord-bundle/scan.ts <buildDir> --code "str1","str2"
   =================================================================== */

function isMainModule(): boolean {
    const entry = process.argv[1];
    if (!entry) return false;
    return path.resolve(entry) === path.resolve(fileURLToPath(import.meta.url));
}

if (isMainModule()) {
    const [, , buildDir, kind, values] = process.argv;
    if (!buildDir || !kind || !values || (kind !== "--props" && kind !== "--code")) {
        console.error(
            "使い方: node scan.ts <buildDir> --props name1,name2  |  node scan.ts <buildDir> --code str1,str2",
        );
        process.exit(1);
    } else {
        const list = values.split(",").filter((s) => s.length > 0);
        const predicate: PredicateSpec =
            kind === "--props" ? { kind: "props", props: list } : { kind: "code", code: list };
        const result = scanOne(buildDir, predicate);
        console.log(JSON.stringify(result, null, 2));
    }
}
