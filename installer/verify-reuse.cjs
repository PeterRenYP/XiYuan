// 验证目录复用、覆盖确认、失败回滚及卸载入口，所有注册与快捷方式均隔离。
const fs=require('node:fs');
const path=require('node:path');
const {spawn,execFileSync}=require('node:child_process');
const net=require('node:net');
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));

async function main(){
  const setup=path.resolve(process.argv[2]),output=path.resolve(process.argv[3]);
  fs.mkdirSync(output,{recursive:false});
  const port=await new Promise(resolve=>{const server=net.createServer();server.listen(0,'127.0.0.1',()=>{const value=server.address().port;server.close(()=>resolve(value));});});
  const child=spawn(setup,['--test'],{windowsHide:true,stdio:'ignore',env:{...process.env,XIYUAN_UI_TEST_ARGUMENTS:'--remote-debugging-port='+port}});
  let socket;const checks={};
  // 断言直接记录验收项，失败时保留原目录供排查。
  function check(value,label){if(!value)throw Error(label);checks[label]=true;}
  try{
    let target;
    for(let i=0;i<200;i++){try{target=(await(await fetch('http://127.0.0.1:'+port+'/json/list')).json()).find(item=>item.type==='page'&&item.url.includes('installer.html'));if(target)break;}catch{}await pause(100);}
    if(!target)throw Error('安装器页面未启动');
    socket=new WebSocket(target.webSocketDebuggerUrl);await new Promise((resolve,reject)=>{socket.addEventListener('open',resolve,{once:true});socket.addEventListener('error',reject,{once:true});});
    let next=1;const pending=new Map();
    socket.addEventListener('message',event=>{const value=JSON.parse(event.data),entry=pending.get(value.id);if(entry){pending.delete(value.id);value.error?entry.reject(Error(JSON.stringify(value.error))):entry.resolve(value.result);}});
    // 每次脚本调用带超时，避免页面异常后永久等待。
    const command=(method,params={})=>new Promise((resolve,reject)=>{const id=next++,timer=setTimeout(()=>{pending.delete(id);reject(Error('页面调用超时'));},10000);pending.set(id,{resolve:value=>{clearTimeout(timer);resolve(value);},reject:error=>{clearTimeout(timer);reject(error);}});socket.send(JSON.stringify({id,method,params}));});
    async function evaluate(expression){const result=await command('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(result.exceptionDetails)throw Error(result.exceptionDetails.exception?.description||result.exceptionDetails.text);return result.result?.value;}
    await command('Runtime.enable');await pause(500);
    await evaluate("window.__events=[];chrome.webview.addEventListener('message',event=>window.__events.push(event.data));");
    // 等待宿主最终事件，提交成功不能代替安装完成。
    async function submit(options){
      await evaluate(`window.__events=[];chrome.webview.postMessage(${JSON.stringify(options)})`);
      for(let i=0;i<600;i++){const event=await evaluate("window.__events.find(event=>['complete','error','busy','overwrite','folder','init'].includes(event.type))");if(event){await pause(450);return event;}await pause(50);}
      throw Error('安装事件超时');
    }
    async function shot(name){const result=await command('Page.captureScreenshot',{format:'png'});fs.writeFileSync(path.join(output,name+'.png'),Buffer.from(result.data,'base64'));}
    const options=directory=>({action:'install',path:directory,desktop:false,menu:true,associate:true});
    const signature=async directory=>{const result=await submit(options(directory));check(result.type==='overwrite','已有程序要求显式确认');return result.signature;};
    const assertNoTemporary=directory=>!fs.readdirSync(path.dirname(directory)).some(name=>name.startsWith(path.basename(directory)+'.install-')||name.startsWith(path.basename(directory)+'.rollback-'));
    for(const [input,browsing,expected] of [['D:\\',true,'D:\\XiYuan'],['D:\\应用',true,'D:\\应用\\XiYuan'],['D:\\应用\\XiYuan',true,'D:\\应用\\XiYuan'],['D:\\应用\\xiyuan',true,'D:\\应用\\xiyuan']]){
      const result=await submit({action:'inspect_path',path:input,browsing});check(result.path===expected,'浏览目录补全且不重复追加：'+input);
    }
    const fresh=path.join(output,'自动创建','XiYuan');
    // 已读过清单后仍须重新检查配套数据包，并允许补齐后在同一窗口重试。
    const dataPackage=setup.replace(/\.exe$/i,'.data'),savedPackage=dataPackage+'.test-backup';
    check((await submit(options(fresh))).type==='complete','双文件安装首次完成');
    fs.renameSync(dataPackage,savedPackage);
    try{
      const result=await submit(options(fresh));
      check(result.type==='error'&&result.message.includes('同一文件夹')&&fs.existsSync(path.join(fresh,'Xiyuan.exe')),'缺少数据包显示中文提示且保留现有安装');
    }finally{fs.renameSync(savedPackage,dataPackage);}
    execFileSync(setup,['--remove-root',fresh,'--test'],{windowsHide:true});
    check((await submit(options(fresh))).type==='complete'&&fs.existsSync(path.join(fresh,'Uninstall.exe')),'不存在的目录自动新建并提供卸载程序');
    const freshManifest=JSON.parse(fs.readFileSync(path.join(fresh,'.xiyuan-install.json'),'utf8'));
    const removal=freshManifest.shortcuts.find(link=>path.basename(link)==='卸载析元.lnk');
    check(removal&&fs.readFileSync(removal).includes(Buffer.from('--uninstall','utf16le')),'开始菜单卸载快捷方式指向卸载命令');
    execFileSync(path.resolve('.venv/Scripts/python.exe'),[path.resolve('tests/verify_uninstall_entry.py'),fresh],{windowsHide:true,timeout:35000});
    check(!fs.existsSync(path.join(fresh,'Xiyuan.exe'))&&!fs.existsSync(removal),'卸载清理程序及开始菜单卸载入口');
    const collision=path.join(output,'文件冲突');fs.mkdirSync(collision);fs.writeFileSync(path.join(collision,'module.dat'),'用户原始文件');
    check((await submit(options(collision))).type==='error'&&fs.readFileSync(path.join(collision,'module.dat'),'utf8')==='用户原始文件','无程序目录中的同名用户文件受到保护');
    const directory=path.join(output,'复用目录');fs.mkdirSync(directory);fs.writeFileSync(path.join(directory,'用户模型.txt'),'模型和项目保留');
    check((await submit(options(directory))).type==='complete'&&fs.readFileSync(path.join(directory,'用户模型.txt'),'utf8')==='模型和项目保留','已有非空目录可直接复用并保留用户文件');
    // 通过实际宿主读回安装记录，旧用户分支同样隔离在本次测试根目录。
    const uninstallKey=`HKCU\\Software\\Xiyuan\\InstallerTests\\${child.pid}\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Xiyuan`;
    const legacyRoot=`HKCU\\Software\\Xiyuan\\InstallerTests\\${child.pid}\\LegacyUser`;
    const registrySet=(key,value)=>execFileSync('reg.exe',['add',key,'/v','InstallLocation','/t','REG_SZ','/d',value,'/f'],{windowsHide:true,stdio:'ignore'});
    const registryDelete=key=>execFileSync('reg.exe',['delete',key,'/f'],{windowsHide:true,stdio:'ignore'});
    let detected=await submit({action:'ready'});
    check(detected.existing&&detected.path===directory,'更新启动自动读取现有自定义安装目录');
    const duplicate=path.join(output,'重复安装');
    const duplicateResult=await submit(options(duplicate));
    check(duplicateResult.type==='error'&&duplicateResult.message.includes(directory)&&!fs.existsSync(duplicate),'已有安装时阻止误装到其他目录并提示原位置');
    registrySet(uninstallKey,path.join(output,'已删除的旧目录'));
    detected=await submit({action:'ready'});check(!detected.existing&&detected.path.endsWith('\\Program Files\\XiYuan'),'失效注册记录不会误判为已有安装');
    registrySet(legacyRoot+'\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Xiyuan',directory);
    detected=await submit({action:'ready'});check(detected.existing&&detected.path===directory,'识别旧用户级安装目录');
    registryDelete(legacyRoot);registrySet(uninstallKey,directory.toUpperCase()+'\\');
    detected=await submit({action:'ready'});check(detected.existing,'安装记录兼容大小写和末尾分隔符');registrySet(uninstallKey,directory);
    // 重载真实页面，检查初始化结果已显示到安装位置输入框。
    await evaluate('location.reload()');await pause(900);
    await evaluate("window.__events=[];chrome.webview.addEventListener('message',event=>window.__events.push(event.data));document.querySelector('[data-action=next]').click()");await pause(450);
    check(await evaluate(`document.querySelector('#xy-path').value===${JSON.stringify(directory)}&&document.querySelector('.xy-subtitle').textContent.includes('已检测到')`),'更新界面默认填入原目录并显示中文说明');await shot('自动识别原安装目录');
    // 将当前小型安装标为旧版，额外加入只属于旧版本的受管文件。
    fs.writeFileSync(path.join(directory,'Xiyuan.exe'),'旧程序');fs.writeFileSync(path.join(directory,'module.dat'),'旧模块');fs.writeFileSync(path.join(directory,'obsolete.dat'),'旧版本文件');
    const manifestPath=path.join(directory,'.xiyuan-install.json'),previous=JSON.parse(fs.readFileSync(manifestPath,'utf8'));previous.files.push({path:'obsolete.dat',size:15,sha256:'0'.repeat(64),blocks:[]});fs.writeFileSync(manifestPath,JSON.stringify(previous));
    let token=await signature(directory);
    check(await evaluate("document.querySelector('#xy-overlay').textContent.includes('此目录已有安装好的程序，继续安装将覆盖原目录程序，确定继续吗？')"),'覆盖提示文案与要求一致');
    await shot('覆盖确认');await evaluate("document.querySelector('#xy-overlay [data-action=dismiss]').click()");
    check(fs.readFileSync(path.join(directory,'Xiyuan.exe'),'utf8')==='旧程序'&&assertNoTemporary(directory),'取消覆盖不修改原程序');
    token=await signature(directory);fs.appendFileSync(path.join(directory,'Xiyuan.exe'),'已变化');
    const stale=await submit({...options(directory),confirm_replace:token});check(stale.type==='overwrite'&&stale.signature!==token,'确认后程序变化必须再次确认');
    const baseline=fs.readFileSync(manifestPath),shortcutBytes=new Map(previous.shortcuts.map(link=>[link,fs.readFileSync(link)]));
    const registryPath=`HKCU\\Software\\Xiyuan\\InstallerTests\\${child.pid}\\Software`;
    const registry=()=>execFileSync('reg.exe',['query',registryPath,'/s'],{windowsHide:true}).toString('base64');
    const originalRegistry=registry();
    // 文件占用和同名暂存目录都必须在改写原程序前停止。
    token=await signature(directory);
    const temporary=directory+'.install-'+child.pid;fs.mkdirSync(temporary);fs.writeFileSync(path.join(temporary,'其他任务.txt'),'不能删除');
    check((await submit({...options(directory),confirm_replace:token})).type==='error'&&fs.readFileSync(path.join(temporary,'其他任务.txt'),'utf8')==='不能删除','不删除其他任务的同名暂存目录');
    fs.unlinkSync(path.join(temporary,'其他任务.txt'));fs.rmdirSync(temporary);
    const lockCode="import ctypes,sys;from ctypes import wintypes;k=ctypes.WinDLL('kernel32',use_last_error=True);k.CreateFileW.argtypes=[wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE];k.CreateFileW.restype=wintypes.HANDLE;k.CloseHandle.argtypes=[wintypes.HANDLE];h=k.CreateFileW(sys.argv[1],0x80000000,1,None,3,0x80,None);assert h not in (None,ctypes.c_void_p(-1).value);print('LOCKED',flush=True);sys.stdin.readline();k.CloseHandle(h)";
    const locker=spawn(path.resolve('.venv/Scripts/python.exe'),['-c',lockCode,path.join(directory,'Xiyuan.exe')],{windowsHide:true,stdio:['pipe','pipe','pipe']});
    try{
      await new Promise((resolve,reject)=>{locker.stdout.once('data',resolve);locker.once('exit',code=>reject(Error('文件锁未启动：'+code)));});
      token=await signature(directory);const busy=await submit({...options(directory),confirm_replace:token});
      check(busy.type==='busy'&&busy.message.includes('正在使用')&&!busy.can_force&&fs.readFileSync(path.join(directory,'Xiyuan.exe'),'utf8')==='旧程序已变化','外部程序锁定文件时保留旧版且不强制关闭无关程序');
      check(await evaluate("document.querySelector('[data-action=retry-install]')&&!document.querySelector('[data-action=force-install]')"),'外部文件占用提供重试并要求手动关闭');
    }finally{locker.stdin.end();await new Promise(resolve=>locker.once('exit',resolve));}
    for(const failure of ['files','registration']){
      token=await signature(directory);
      const result=await submit({...options(directory),confirm_replace:token,test_failure:failure});
      fs.writeFileSync(path.join(output,'失败事件-'+failure+'.json'),JSON.stringify(result,null,2));
      check(result.type==='error'&&fs.readFileSync(path.join(directory,'Xiyuan.exe'),'utf8')==='旧程序已变化'&&fs.readFileSync(path.join(directory,'module.dat'),'utf8')==='旧模块'&&fs.existsSync(path.join(directory,'obsolete.dat'))&&baseline.equals(fs.readFileSync(manifestPath)),'失败恢复旧程序和安装清单：'+failure);
      check(registry()===originalRegistry&&[...shortcutBytes].every(([file,bytes])=>bytes.equals(fs.readFileSync(file))),'失败恢复原注册信息与快捷方式：'+failure);
      check(assertNoTemporary(directory)&&fs.readFileSync(path.join(directory,'用户模型.txt'),'utf8')==='模型和项目保留','失败清理暂存并保留用户数据：'+failure);
    }
    token=await signature(directory);await evaluate("document.querySelector('#xy-overlay [data-action=confirm-overwrite]').click()");
    let finished=false;for(let i=0;i<600;i++){if(await evaluate("window.__events.some(event=>event.type==='complete')")){finished=true;break;}await pause(50);}
    check(finished&&fs.readFileSync(path.join(directory,'Xiyuan.exe')).readUInt16LE(0)===0x5a4d&&fs.readFileSync(path.join(directory,'module.dat'),'utf8')==='new-program-data','点击继续安装完成程序覆盖');
    check(!fs.existsSync(path.join(directory,'obsolete.dat'))&&fs.readFileSync(path.join(directory,'用户模型.txt'),'utf8')==='模型和项目保留'&&assertNoTemporary(directory),'覆盖清理旧程序文件并保留用户数据');
    await pause(450);await shot('覆盖完成');
    // 只读文件不是进程占用，错误必须显示真实原因。
    const modulePath=path.join(directory,'module.dat');execFileSync('attrib.exe',['+R',modulePath],{windowsHide:true});
    try{token=await signature(directory);const result=await submit({...options(directory),confirm_replace:token});check(result.type==='error'&&result.message.includes('只读')&&!result.message.includes('正在使用'),'只读文件不会误报为程序正在使用');}
    finally{execFileSync('attrib.exe',['-R',modulePath],{windowsHide:true});}
    // 小型验收载荷中的可执行程序只等待退出，模拟无窗口主程序及后台运行时。
    const runtime=path.join(directory,'ModelObservatory-Runtime.exe');fs.copyFileSync(path.join(directory,'Xiyuan.exe'),runtime);
    const siblingDirectory=path.join(output,'其他位置');fs.mkdirSync(siblingDirectory);const sibling=path.join(siblingDirectory,'Xiyuan.exe');fs.copyFileSync(path.join(directory,'Xiyuan.exe'),sibling);
    const background=spawn(runtime,[],{windowsHide:true,stdio:'ignore'}),mainProgram=spawn(path.join(directory,'Xiyuan.exe'),[],{windowsHide:true,stdio:'ignore'}),unrelated=spawn(sibling,[],{windowsHide:true,stdio:'ignore'});
    try{
      await pause(300);check(background.exitCode===null&&mainProgram.exitCode===null&&unrelated.exitCode===null,'启动隔离的后台进程和其他位置同名程序');
      token=await signature(directory);let busy=await submit({...options(directory),confirm_replace:token});
      check(busy.type==='busy'&&busy.can_force&&busy.programs.some(p=>p.pid===background.pid)&&busy.programs.some(p=>p.pid===mainProgram.pid)&&!busy.programs.some(p=>p.pid===unrelated.pid),'识别无窗口主程序与运行时且按路径排除同名程序');
      await shot('后台进程处理');
      check(await evaluate("document.activeElement.dataset.action==='dismiss'&&document.querySelector('.xy-dialog').textContent.includes('未保存')"),'强制关闭前提示未保存内容并默认聚焦返回');
      await evaluate("document.querySelector('[data-action=dismiss]').click()");check(background.exitCode===null&&mainProgram.exitCode===null,'取消占用处理不会关闭程序');
      token=await signature(directory);await evaluate("window.__events=[];document.querySelector('[data-action=confirm-overwrite]').click()");
      for(let i=0;i<200;i++){if(await evaluate("window.__events.some(e=>e.type==='busy')"))break;await pause(50);}await pause(450);
      await evaluate("window.__events=[];document.querySelector('[data-action=force-install]').click()");
      let completed=false;for(let i=0;i<600;i++){const result=await evaluate("window.__events.find(e=>['complete','error','busy','overwrite'].includes(e.type))");if(result){check(result.type==='complete','强制关闭后实际完成覆盖安装');completed=true;break;}await pause(50);}
      check(completed&&background.exitCode!==null&&mainProgram.exitCode!==null&&unrelated.exitCode===null,'只关闭目标目录的进程且其他位置程序继续运行');
      const installed=JSON.parse(fs.readFileSync(manifestPath,'utf8'));check(!('force_close' in installed.options)&&fs.readFileSync(path.join(directory,'用户模型.txt'),'utf8')==='模型和项目保留','强制关闭选项不持久化且用户文件保留');
      fs.unlinkSync(runtime);
    }finally{for(const processHandle of [background,mainProgram,unrelated])if(processHandle.exitCode===null)processHandle.kill();}
    execFileSync(setup,['--remove-root',directory,'--test'],{windowsHide:true});
    check(fs.readFileSync(path.join(directory,'用户模型.txt'),'utf8')==='模型和项目保留'&&!fs.existsSync(path.join(directory,'Xiyuan.exe')),'覆盖安装后仍可卸载并保留用户数据');
    fs.writeFileSync(path.join(output,'目录复用验收.json'),JSON.stringify({pass:true,checks},null,2));console.log(JSON.stringify(checks));
  }finally{socket?.close();if(child.exitCode===null)child.kill();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
