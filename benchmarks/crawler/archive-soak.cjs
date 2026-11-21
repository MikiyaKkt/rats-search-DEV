// Preserve content-free results/resources; diagnostic stdout/stderr stay local.
const fs=require('fs'),path=require('path'),crypto=require('crypto'),zlib=require('zlib');
const {pipeline}=require('stream/promises');
const control=path.resolve(process.argv[2]||'build-windows-x64/p5-two-hour-final');
const network=path.resolve(process.argv[3]||'build-windows-x64/p5-network-two-hour');
const output=path.resolve(process.argv[4]||'benchmarks/crawler/results/p5');
async function digest(stream){const hash=crypto.createHash('sha256');for await(const chunk of stream)hash.update(chunk);return hash.digest('hex');}
async function copy(source,name,gzip=false){
    const target=path.join(output,name);
    if(fs.existsSync(target))throw Error('Refusing to overwrite '+target);
    const originalSha256=await digest(fs.createReadStream(source));
    if(gzip)await pipeline(fs.createReadStream(source),zlib.createGzip({level:9}),fs.createWriteStream(target,{flags:'wx'}));
    else await fs.promises.copyFile(source,target,fs.constants.COPYFILE_EXCL);
    const restored=gzip?fs.createReadStream(target).pipe(zlib.createGunzip()):fs.createReadStream(target);
    if(await digest(restored)!==originalSha256)throw Error('Archive verification failed: '+name);
    return{source:path.relative(process.cwd(),source).replaceAll('\\','/'),file:name,
        sourceBytes:fs.statSync(source).size,archiveBytes:fs.statSync(target).size,sourceSha256:originalSha256,
        archiveSha256:await digest(fs.createReadStream(target)),roundTripVerified:true};
}
(async()=>{
    for(const root of [control,network]){
        const m=JSON.parse(fs.readFileSync(path.join(root,'run.json'),'utf8').replace(/^\uFEFF/,''));
        if(!m.completed||m.cancelled)throw Error('Only normally completed runs may be archived: '+root);
    }
    fs.mkdirSync(output,{recursive:true});
    const entries=[];
    for(const [root,prefix,probe] of [[control,'controlled','controlled'],[network,'network','live']]){
        entries.push(await copy(path.join(root,'run.json'),prefix+'-run.json'));
        entries.push(await copy(path.join(root,probe+'.json'),prefix+'.json'));
        entries.push(await copy(path.join(root,probe+'.json.resources.jsonl'),prefix+'.resources.jsonl.gz',true));
    }
    const index={phase:'P5',utc:new Date().toISOString(),entries,
        scope:'Probe result JSON and streamed process resources only. Raw files remain in the build directory; diagnostic logs are excluded.'};
    fs.writeFileSync(path.join(output,'archive.json'),JSON.stringify(index,null,2)+'\n',{flag:'wx'});
    console.log(JSON.stringify(index,null,2));
})().catch(e=>{console.error(e.message);process.exitCode=1;});
