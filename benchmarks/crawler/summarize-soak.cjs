const fs=require('fs'),path=require('path'),readline=require('readline');
const directory=path.resolve(process.argv[2]||'build-windows-x64/p5-two-hour-final');
const output=path.resolve(process.argv[3]||'benchmarks/crawler/results/p5-soak-summary.json');
const networkDirectory=process.argv[4]?path.resolve(process.argv[4]):directory;
const read=p=>JSON.parse(fs.readFileSync(p,'utf8').replace(/^\uFEFF/,''));
function window(){return{count:0,rss:0,private:0,threads:0};}
function add(w,row){++w.count;w.rss+=row.working_set_bytes;w.private+=row.private_bytes;w.threads+=row.threads;}
function mean(w){return w.count?{samples:w.count,workingSetMiB:w.rss/w.count/1048576,privateMiB:w.private/w.count/1048576,threads:w.threads/w.count}:null;}
async function resources(file,durationMs,controlled){
    let count=0,first,last,maxGap=0,peakThreads=0,peakRss=0,osPeakRss=0,peakPrivate=0;
    let pendingPeak=0,activePeak=0,estimatedPeak=0,connectionPeak=0;
    const startWindow=window(),endWindow=window();
    const warmupMs=Math.min(900000,durationMs/4),windowMs=Math.min(900000,durationMs/4);
    let n=0,sx=0,sy=0,sxx=0,sxy=0;
    for await(const line of readline.createInterface({input:fs.createReadStream(file),crlfDelay:Infinity})){
        if(!line.trim())continue;const r=JSON.parse(line);++count;if(!first)first=r;
        if(last)maxGap=Math.max(maxGap,r.elapsed_ms-last.elapsed_ms);last=r;
        peakThreads=Math.max(peakThreads,r.threads);peakRss=Math.max(peakRss,r.working_set_bytes);
        osPeakRss=Math.max(osPeakRss,r.peak_working_set_bytes||0);peakPrivate=Math.max(peakPrivate,r.private_bytes);
        if(r.elapsed_ms>=warmupMs&&r.elapsed_ms<warmupMs+windowMs)add(startWindow,r);
        if(r.elapsed_ms>=durationMs-windowMs)add(endWindow,r);
        if(r.elapsed_ms>=warmupMs){const x=r.elapsed_ms/1000,y=r.private_bytes/1048576;++n;sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;}
        if(controlled){
            pendingPeak=Math.max(pendingPeak,r.pending);activePeak=Math.max(activePeak,r.active);connectionPeak=Math.max(connectionPeak,r.connections);
            const bytes=r.metrics.gauges['crawler.db_pending_bytes'].current;estimatedPeak=Math.max(estimatedPeak,bytes);
            if(r.pending<0||r.pending>1000||r.active<0||r.active>64||bytes<0||bytes>64*1048576)throw Error('Capacity violated in '+file);
        }
    }
    if(!count)throw Error('No resource samples: '+file);
    return{file,samples:count,firstElapsedMs:first.elapsed_ms,lastElapsedMs:last.elapsed_ms,maxSampleGapMs:maxGap,
        coverageRatio:(last.elapsed_ms-first.elapsed_ms)/durationMs,samplingDensity:count/(durationMs/1000),
        baselineThreads:first.threads,lastThreads:last.threads,peakThreads,peakSampledWorkingSetBytes:peakRss,peakOsWorkingSetBytes:osPeakRss,
        peakPrivateBytes:peakPrivate,cpuCorePercent:(last.cpu_seconds-first.cpu_seconds)*100/((last.elapsed_ms-first.elapsed_ms)/1000),
        warmupMs,windowMs,firstSteadyWindow:mean(startWindow),lastSteadyWindow:mean(endWindow),
        privateSlopeMiBPerHour:n*sxx-sx*sx?3600*(n*sxy-sx*sy)/(n*sxx-sx*sx):null,
        ...(controlled?{pendingPeak,activePeak,estimatedBytesPeak:estimatedPeak,connectionPeak}:{})};
}
(async()=>{
    const manifest=read(path.join(directory,'run.json'));
    if(!manifest.completed||manifest.cancelled)throw Error('Both probes must have completed normally');
    const networkManifest=read(path.join(networkDirectory,'run.json'));
    if(!networkManifest.completed||networkManifest.cancelled||networkManifest.seconds!==manifest.seconds
        ||networkManifest.cycles!==manifest.cycles||networkManifest.live_sha256!==manifest.live_sha256)throw Error('Network run/version mismatch');
    const controlled=read(path.join(directory,'controlled.json')),live=read(path.join(networkDirectory,'live.json'));
    if(controlled.failed||controlled.cancelled||live.cancelled||!controlled.capacity_accounted
        ||controlled.wall_ms<manifest.seconds*1000||live.wall_ms<manifest.seconds*1000)throw Error('Cancelled, short or failed run');
    if(controlled.restart_cycles.length!==manifest.cycles||live.restart_cycles.length!==manifest.cycles)throw Error('Missing restart cycles');
    if(controlled.connections_after_stop!==0||live.connections_after_stop!==0)throw Error('Connections remain after stop');
    if(!controlled.statistics_exact||controlled.database_rows!==controlled.statistics_rows)throw Error('Database count mismatch');
    if(controlled.restart_cycles.some(r=>r.stopped_connections!==controlled.baseline_connections)
        ||live.restart_cycles.some(r=>!r.running||r.stopped_connections!==live.restart_cycles[0].stopped_connections))throw Error('Restart invariant failed');
    const counters=controlled.final_metrics.counters;
    if(controlled.final_metrics.gauges['crawler.db_pending'].current!==0||controlled.final_metrics.gauges['crawler.metadata_inflight'].current!==0)throw Error('Controlled capacity did not drain');
    for(const key of ['crawler.db_pending','crawler.metadata_inflight','crawler.discovery_slots'])
        if((live.post_stop_snapshot.gauges[key]?.current||0)!==0)throw Error('Live capacity did not drain: '+key);
    const liveStart=live.start_snapshot.counters||{},liveEnd=live.end_snapshot.counters||{};
    const delta=key=>(liveEnd[key]||0)-(liveStart[key]||0);
    const summary={phase:'P5',wallClockRun:true,secondsRequested:manifest.seconds,longValidation:manifest.seconds>=7200&&manifest.cycles>=100,
        cyclesPerProbe:manifest.cycles,controlled:{wallMs:controlled.wall_ms,delivered:controlled.delivered,databaseRows:controlled.database_rows,
            exact:true,capacityAccounted:true,connectionsAfterStop:0,
            accepted:counters['crawler.capacity_accepted'],completed:counters['crawler.capacity_completed'],cancelled:counters['crawler.capacity_cancelled'],
            databaseRetries:counters['crawler.db_retries']||0,networkRetries:counters['crawler.network_retries']||0,
            metricPeaks:Object.fromEntries(['crawler.db_pending','crawler.db_pending_bytes','crawler.metadata_inflight','crawler.discovery_slots']
                .map(key=>[key,controlled.final_metrics.gauges[key]?.peak||0])),
            resources:await resources(path.join(directory,'controlled.json.resources.jsonl'),manifest.seconds*1000,true)},
        live:{wallMs:live.wall_ms,announcements:delta('crawler.announcements'),metadataAttempts:delta('crawler.metadata_attempts'),
            metadataSuccesses:delta('crawler.metadata_succeeded'),metadataFailures:delta('crawler.metadata_failed'),
            metadataTimeouts:delta('crawler.metadata_timeouts'),networkRetries:delta('crawler.network_retries'),
            inserted:delta('crawler.index_inserted'),dhtNodesAtEnd:live.dht_nodes_at_end,
            connectionsAfterStop:0,resourceSamplesRetained:live.resources.length,resourceSamplesWritten:live.resource_samples,
            executionEnvironment:networkDirectory===directory?'same environment as controlled probe':'separate network environment',
            resources:await resources(path.join(networkDirectory,'live.json.resources.jsonl'),manifest.seconds*1000,false)},
        binarySha256:{controlled:manifest.controlled_sha256,live:manifest.live_sha256},
        caveats:['Controlled metadata availability and intermittent database errors are injected; not real DHT throughput.',
            'Resources cover each probe only, excluding searchd and system-wide traffic; sampling itself adds overhead.',
            'Steady-window resource changes describe this workload and duration; they do not prove absence of all leaks.',
            'Confirmed rows may outnumber notifications when a restart cancels delivery after commit.',
            'No wire bandwidth measurement.'],networkBandwidth:null};
    summary.continuousSampling=[summary.controlled.resources,summary.live.resources].every(r=>
        r.maxSampleGapMs<=10000&&r.coverageRatio>=0.95&&r.samplingDensity>=0.9);
    summary.longValidation=summary.longValidation&&summary.continuousSampling;
    fs.mkdirSync(path.dirname(output),{recursive:true});fs.writeFileSync(output,JSON.stringify(summary,null,2)+'\n');
    console.log(JSON.stringify(summary,null,2));
})().catch(e=>{console.error(e.message);process.exitCode=1;});
