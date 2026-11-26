const assert = require('node:assert/strict');
const { execFileSync, spawn } = require('node:child_process');
const { once } = require('node:events');
const fs = require('node:fs/promises');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { performance } = require('node:perf_hooks');
const { setTimeout: delay } = require('node:timers/promises');

const root = path.resolve(__dirname, '../..');
const bundledSearchd = path.join(root, 'imports/win/x64/searchd.exe');
const settings = 'charset_table = non_cont\n    ngram_len = 1\n    ngram_chars = cont';
const batchSize = 250;
const fillerCount = 10000;
const warmupRounds = 5;
const measuredRounds = 40;

const fixture = [
    [1, '经典电影合集 1080p', ['馆藏/经典电影合集.1080p.mkv']],
    [2, '电台摄影 1080p', ['馆藏/电台摄影.1080p.mkv']],
    [3, '影电 1080p', ['馆藏/影电.1080p.mkv']],
    [4, '电影附件', ['馆藏/电影.mkv', '画质/1080p.txt']],
    [5, '星际 穿越 导演剪辑版', ['片库/星际 穿越.mkv']],
    [6, '星际 经典 穿越', ['片库/星际 经典 穿越.mkv']],
    [7, '跨行样本', ['分类/星际', '穿越.mkv']],
    [8, '音楽集', ['作品/音楽集.flac']],
    [9, '音の楽', ['作品/音の楽.flac']],
    [10, '한국영화모음', ['목록/한국영화모음.mkv']],
    [11, '영과화', ['목록/영과화.mkv']],
    [12, 'เพลงไทยเก่า', ['รายการ/เพลงไทยเก่า.flac']],
    [13, 'เพลกง', ['รายการ/เพลกง.flac']],
    [14, 'Ubuntu Linux Archive', ['software/Ubuntu Linux.iso']],
    [15, 'Ubuntux Linux Archive', ['software/Ubuntux Linux.iso']],
    [16, 'Ubuntu Modern Linux', ['software/Ubuntu-Modern-Linux.iso']],
    [17, 'UbuntuX LLVM Archive', ['software/UbuntuX LLVM.iso']],
    [18, '电-影', ['馆藏/电-影.mkv']],
    [19, '星际独立文件', ['星际', '穿越']],
    [20, '1080p 电影', ['馆藏/1080p 电影.mkv']],
    [21, '历史资料', ['资料/旧文件.txt'], '经典电影'],
    [22, '仅路径命中', ['资料/导演剪辑版.mkv', '资料/剧照.jpg']],
];

function corpusRow([id, name, paths, alternateName]) {
    return {
        id,
        hash: id.toString(16).padStart(40, '0'),
        name,
        paths,
        nameIndex: alternateName ? name + ' ' + alternateName : name,
        size: id * 1000 * paths.length,
        contentType: id % 2 + 1,
        contentCategory: id % 3 + 1,
        seeders: id % 23,
    };
}

const samples = fixture.map(corpusRow);

function makeCorpus() {
    const names = ['Digital Archive', '科学資料', '古典문화', 'แผนที่โลก', 'Weather Records'];
    const rows = [...samples];
    for (let offset = 0; offset < fillerCount; offset++) {
        const id = 1000 + offset;
        const name = names[offset % names.length] + ' catalog' + Math.floor(offset / names.length);
        const paths = Array.from({ length: 5 }, (_, index) => `collection${offset % 37}/${name}/entry${index}.dat`);
        rows.push(corpusRow([id, name, paths]));
    }
    return rows;
}

function sqlString(value) {
    const escaped = value.replaceAll('\\', '\\\\').replaceAll("'", "\\'").replaceAll('\n', '\\n');
    return "'" + escaped + "'";
}

function currentMatch(userText) {
    return userText.replace(/[\\!"$()\-/<@^|~]/g, '\\$&');
}

function searchSql(table, match, { snippet, snippetText = match, option = '', maxId = samples.length } = {}) {
    let columns = 'id, hash';
    if (snippet === 'legacy') {
        columns += `, SNIPPET(path, ${sqlString(snippetText)}, 'around=100', 'force_all_words=1') AS snippet`;
    } else if (snippet === 'match') {
        columns += ', HIGHLIGHT() AS snippet';
    }
    return `SELECT ${columns} FROM ${table} WHERE MATCH(${sqlString(match)}) AND id <= ${maxId} ORDER BY id ASC LIMIT 20${option}`;
}

const cases = [
    { label: 'zh-characters-plain', raw: '电影', expected: { torrents: [1, 4, 20, 21], files: [1, 4, 20] } },
    { label: 'zh-contiguous', raw: '电影', match: '"电影"', expected: { torrents: [1, 4, 20, 21], files: [1, 4, 20] } },
    { label: 'ja-contiguous', raw: '音楽', match: '"音楽"', expected: { torrents: [8], files: [8] } },
    { label: 'ko-contiguous', raw: '영화', match: '"영화"', expected: { torrents: [10], files: [10] } },
    { label: 'th-contiguous', raw: 'เพลง', match: '"เพลง"', expected: { torrents: [12], files: [12] } },
    { label: 'zh-mixed-and', raw: '电影 1080p', match: '"电影" 1080p', expected: { torrents: [1, 20], files: [1, 20] } },
    { label: 'zh-mixed-ideographic-space', raw: '1080p　电影', match: '1080p "电影"', expected: { torrents: [1, 20], files: [1, 20] } },
    { label: 'zh-phrase', raw: '"星际 穿越"', match: '"星际 穿越"', expected: { torrents: [5], files: [5] } },
    { label: 'zh-path-only', raw: '导演剪辑版', match: '"导演剪辑版"', expected: { torrents: [5], files: [22] } },
    { label: 'en-and', raw: 'Ubuntu Linux', expected: { torrents: [14, 15, 16], files: [14, 16] } },
    { label: 'en-implicit-prefix', raw: 'ubun', expected: { torrents: [14, 15, 16, 17], files: [] } },
    { label: 'en-explicit-prefix', raw: 'ubun*', expected: { torrents: [14, 15, 16, 17], files: [] } },
    { label: 'en-phrase', raw: '"Ubuntu Linux"', match: '"Ubuntu Linux"', expected: { torrents: [14], files: [14] } },
    { label: 'en-phrase-no-expansion', raw: '"Ubuntu Linux"', match: '"Ubuntu Linux"', option: ' OPTION expand_keywords=0', expected: { torrents: [14], files: [14] } },
    { label: 'en-phrase-with-star', raw: '"ubun* linux"', match: '"ubun* linux"', expected: { torrents: [], files: [] } },
    { label: 'en-escaped-star-in-phrase', raw: '"ubun* linux"', match: '"ubun\\* linux"', option: ' OPTION expand_keywords=0', expected: { torrents: [], files: [] } },
    { label: 'en-phrase-star-as-separator', raw: '"ubun* linux"', match: '"ubun linux"', option: ' OPTION expand_keywords=0', expected: { torrents: [], files: [] } },
    { label: 'en-phrase-with-explicit-prefix-term', raw: '"Ubuntu Linux" archiv', match: '"Ubuntu Linux" (archiv | archiv*)', option: ' OPTION expand_keywords=0', expected: { torrents: [14], files: [] } },
];

async function sql(port, statement) {
    const response = await fetch(`http://127.0.0.1:${port}/sql?mode=raw`, {
        method: 'POST',
        body: statement,
        signal: AbortSignal.timeout(10000),
    });
    const payload = await response.json();
    const result = payload[0];
    if (!response.ok || !result || result.error) {
        throw new Error(result?.error || payload.error || `Manticore HTTP ${response.status}`);
    }
    return result.data || [];
}

async function findPort() {
    const listener = net.createServer();
    listener.listen(0, '127.0.0.1');
    await once(listener, 'listening');
    const { port } = listener.address();
    await new Promise((resolve, reject) => listener.close(error => error ? reject(error) : resolve()));
    return port;
}

async function withIndex(binary, directory, definitions, useNgrams, action) {
    const port = await findPort();
    await fs.mkdir(directory);
    const configDir = directory.replaceAll('\\', '/');
    const config = path.join(directory, 'manticore.conf');
    const indexes = definitions.map(definition => {
        const local = definition.replace('%1', configDir);
        return useNgrams ? local.replace(/\n}/, `\n    ${settings}\n}`) : local;
    });
    await fs.writeFile(config, indexes.join('\n\n') + `
searchd
{
    listen = 127.0.0.1:${port}:http
    pid_file = ${configDir}/searchd.pid
    binlog_path = ${configDir}
}
`);
    const child = spawn(binary, ['--config', config, '--console'], { cwd: directory, windowsHide: true });
    let log = '';
    let startError;
    child.on('error', error => { startError = error; });
    child.stdout.on('data', data => { log += data; });
    child.stderr.on('data', data => { log += data; });
    const closed = new Promise(resolve => child.once('close', resolve));
    try {
        const deadline = performance.now() + 15000;
        while (!/accepting connections/i.test(log)) {
            if (startError || child.exitCode !== null || performance.now() > deadline) {
                throw new Error(`searchd could not start: ${startError || log}`);
            }
            await delay(50);
        }
        const query = statement => sql(port, statement);
        assert.deepEqual((await query('SHOW TABLES')).map(row => row.Index || row.Table).sort(), ['files', 'torrents']);
        return await action(query, directory);
    } finally {
        if (child.pid && child.exitCode === null) child.kill();
        await closed;
        await fs.writeFile(path.join(directory, 'searchd.log'), log);
    }
}

function summarizeTimes(values) {
    const sorted = values.toSorted((left, right) => left - right);
    const quantile = fraction => Number(sorted[Math.ceil(fraction * sorted.length) - 1].toFixed(3));
    return { count: values.length, p50Ms: quantile(0.5), p95Ms: quantile(0.95) };
}

async function measureIndex(query, directory, rows, useNgrams) {
    const started = performance.now();
    for (let offset = 0; offset < rows.length; offset += batchSize) {
        const batch = rows.slice(offset, offset + batchSize);
        const titles = batch.map(row => `(${row.id},${sqlString(row.hash)},${sqlString(row.name)},${sqlString(row.nameIndex)},${row.size},${row.paths.length},${row.contentType},${row.contentCategory},${row.seeders})`);
        const files = batch.map(row => {
            const sizes = row.paths.map(() => String(row.size / row.paths.length));
            return `(${row.id},${sqlString(row.hash)},${sqlString(row.paths.join('\n'))},${sqlString(sizes.join('\n'))})`;
        });
        await query(`INSERT INTO torrents (id,hash,name,nameIndex,size,files,contentType,contentCategory,seeders) VALUES ${titles.join(',')}`);
        await query(`INSERT INTO files (id,hash,path,size) VALUES ${files.join(',')}`);
    }
    const loadMs = Number((performance.now() - started).toFixed(1));
    await query('FLUSH RAMCHUNK torrents');
    await query('FLUSH RAMCHUNK files');
    const indexBytes = { torrents: 0, files: 0 };
    for (const file of await fs.readdir(directory)) {
        const prefix = file.split('.')[0];
        if (prefix in indexBytes) indexBytes[prefix] += (await fs.stat(path.join(directory, file))).size;
    }
    for (const table of ['torrents', 'files']) {
        const [count] = await query(`SELECT count(*) AS n FROM ${table}`);
        assert.equal(Number(count.n), rows.length, `unexpected row count in ${table}`);
    }

    const observed = [];
    for (const test of cases) {
        const match = useNgrams ? test.match || test.raw : currentMatch(test.raw);
        const result = { label: test.label, match, option: useNgrams ? test.option : undefined };
        for (const table of ['torrents', 'files']) {
            try {
                const matches = await query(searchSql(table, match, {
                    option: result.option,
                    snippet: table === 'files' ? (useNgrams ? 'match' : 'legacy') : undefined,
                    snippetText: test.raw,
                }));
                result[table] = matches.map(row => ({
                    id: Number(row.id), hash: row.hash, snippet: row.snippet,
                }));
            } catch (error) {
                result[table] = { error: error.message };
            }
        }
        observed.push(result);
    }
    const tokenPositions = {};
    for (const text of ['电影', '电-影', '音楽', '영화', 'เพลง', 'Ubuntu Linux', '星际\n穿越']) {
        tokenPositions[text] = await query(`CALL KEYWORDS(${sqlString(text)}, 'torrents')`);
    }
    const legacySnippets = await query(searchSql('files', '"电影" 1080p', { snippet: 'legacy', snippetText: '电影 1080p' }));
    const snippetOptions = '100 AS around, 1 AS force_all_words, 1 AS allow_empty';
    const snippetCases = [
        ['"电影" 1080p', ['馆藏/经典电影合集.1080p.mkv', '馆藏/电影.mkv', '画质/1080p.txt', '馆藏/1080p 电影.mkv']],
        ['"星际 穿越"', ['片库/星际 穿越.mkv', '片库/星际 经典 穿越.mkv', '分类/星际', '穿越.mkv']],
        ['"电影"', ['馆藏/经典电影合集.mkv', '馆藏/电-影.mkv', '馆藏/影电.mkv']],
    ];
    const isolatedSnippets = [];
    for (const [match, paths] of snippetCases) {
        const snippets = await query(
            `CALL SNIPPETS((${paths.map(sqlString).join(',')}), 'files', ${sqlString(match)}, ${snippetOptions})`);
        assert.equal(snippets.length, paths.length);
        isolatedSnippets.push({
            match,
            paths: paths.map((filePath, index) => ({ path: filePath, snippet: snippets[index].snippet })),
        });
    }
    const benchmark = [
        ['torrents', 'catalog1'], ['torrents', 'catalog*'], ['torrents', '科学資料'], ['torrents', '电影 1080p'],
        ['files', 'entry2'], ['files', 'Digital Archive'], ['files', '科学資料 entry2'], ['files', '电影 1080p'],
    ];
    const times = benchmark.map(() => []);
    for (let round = 0; round < warmupRounds + measuredRounds; round++) {
        for (const [index, [table, match]] of benchmark.entries()) {
            const statement = searchSql(table, match, {
                maxId: 1000 + fillerCount,
                snippet: table === 'files' ? 'legacy' : undefined,
            });
            const before = performance.now();
            await query(statement);
            if (round >= warmupRounds) times[index].push(performance.now() - before);
        }
    }
    const latency = {};
    for (const table of ['torrents', 'files']) {
        latency[table] = summarizeTimes(times.flatMap((values, index) => benchmark[index][0] === table ? values : []));
    }
    return {
        loadMs, indexBytes, observed, tokenPositions, legacySnippets,
        isolatedSnippets,
        latency,
        workload: benchmark.map(([table, match], index) => ({ table, match, ...summarizeTimes(times[index]) })),
    };
}

async function main() {
    const binary = path.resolve(process.argv[2] || bundledSearchd);
    assert(process.argv[2] || (process.platform === 'win32' && process.arch === 'x64'),
        'Pass the path to the platform searchd executable.');
    const version = execFileSync(binary, ['--version'], { encoding: 'utf8', windowsHide: true }).split(/\r?\n/)[0];
    const source = execFileSync('git', ['show', '3965c28:src/data/manticore.cpp'], { cwd: root, encoding: 'utf8' });
    const definitions = [...source.matchAll(/^index (?:torrents|files)\r?\n\{[\s\S]*?^}/gm)]
        .map(match => match[0].replaceAll('\r\n', '\n'));
    assert.equal(definitions.length, 2);
    const temporary = path.join(root, '.tools');
    await fs.mkdir(temporary, { recursive: true });
    const directory = await fs.mkdtemp(path.join(temporary, 'search-phase1-'));
    console.log(`Phase 1 workspace: ${directory}`);
    const rows = makeCorpus();
    const report = {
        version,
        platform: `${process.platform} ${process.arch} ${os.release()}`,
        cpu: os.cpus()[0].model,
        node: process.version,
        settings,
        batchSize,
        warmupRounds,
        measuredRounds,
        corpus: { torrents: rows.length, paths: rows.reduce((count, row) => count + row.paths.length, 0) },
        samples,
        cases,
    };
    for (const [name, useNgrams] of [['baseline', false], ['ngrams', true]]) {
        report[name] = await withIndex(binary, path.join(directory, name), definitions, useNgrams,
            (query, target) => measureIndex(query, target, rows, useNgrams));
        console.log(`${name}: ${JSON.stringify(report[name].latency)}`);
    }
    await fs.writeFile(path.join(directory, 'result.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(path.join(directory, 'result.json'));

    for (const result of [report.baseline, report.ngrams]) {
        for (const observation of result.observed) {
            assert(Array.isArray(observation.torrents), JSON.stringify(observation));
            assert(Array.isArray(observation.files), JSON.stringify(observation));
        }
    }
    for (const label of ['ja-contiguous', 'ko-contiguous', 'th-contiguous', 'zh-path-only',
        'en-and', 'en-implicit-prefix', 'en-explicit-prefix', 'en-phrase-no-expansion', 'en-phrase-star-as-separator',
        'en-phrase-with-explicit-prefix-term']) {
        const expected = cases.find(test => test.label === label).expected;
        const observed = report.ngrams.observed.find(result => result.label === label);
        for (const table of ['torrents', 'files']) {
            assert.deepEqual(observed[table].map(row => row.id), expected[table], `${label} in ${table}`);
        }
    }
    for (const label of ['en-and', 'en-implicit-prefix', 'en-explicit-prefix']) {
        const expected = cases.find(test => test.label === label).expected;
        const observed = report.baseline.observed.find(result => result.label === label);
        for (const table of ['torrents', 'files']) {
            assert.deepEqual(observed[table].map(row => row.id), expected[table], `baseline ${label} in ${table}`);
        }
    }

    for (const [index, expected] of [[0, [true, false, false, true]], [1, [true, false, false, false]]]) {
        const probe = report.ngrams.isolatedSnippets[index];
        assert.deepEqual(probe.paths.map(row => row.snippet.includes('<b>')), expected, probe.match);
    }
    const contiguous = report.ngrams.observed.find(result => result.label === 'zh-contiguous');
    const expected = cases.find(test => test.label === 'zh-contiguous').expected;
    const literalTitles = contiguous.torrents.filter(row => samples[row.id - 1].nameIndex.includes('电影'));
    const literalFiles = contiguous.files.filter(row => samples[row.id - 1].paths.some(filePath => filePath.includes('电影')));
    assert.deepEqual(literalTitles.map(row => row.id), expected.torrents);
    assert.deepEqual(literalFiles.map(row => row.id), expected.files);
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
