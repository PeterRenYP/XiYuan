// 对本项目安装器进行可重复的界面联调，注册信息写入隔离测试分支。
const fs=require('node:fs');
const path=require('node:path');
const {spawn}=require('node:child_process');
const net=require('node:net');
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));

async function main(){
  const output=path.resolve(process.argv[3]);fs.mkdirSync(output,{recursive:true});
  const port=await new Promise(resolve=>{const server=net.createServer();server.listen(0,'127.0.0.1',()=>{const p=server.address().port;server.close(()=>resolve(p));});});
  const processHandle=spawn(path.resolve(process.argv[2]),['--test'],{windowsHide:true,env:{...process.env,XIYUAN_UI_TEST_ARGUMENTS:'--remote-debugging-port='+port},stdio:'ignore'});
  let socket;
  const checks={};const errors=[];
  try{
    let target;
    for(let i=0;i<150;i++){try{const targets=await(await fetch('http://127.0.0.1:'+port+'/json/list')).json();target=targets.find(t=>t.type==='page'&&t.url.includes('installer.html'));if(target)break;}catch{}await pause(100);}
    if(!target)throw Error('未启动安装页面');
    socket=new WebSocket(target.webSocketDebuggerUrl);await new Promise((resolve,reject)=>{socket.addEventListener('open',resolve,{once:true});socket.addEventListener('error',reject,{once:true});});
    let next=1;const pending=new Map();
    socket.addEventListener('message',event=>{const message=JSON.parse(event.data);if(pending.has(message.id)){const p=pending.get(message.id);pending.delete(message.id);message.error?p.reject(Error(JSON.stringify(message.error))):p.resolve(message.result);}if(message.method==='Runtime.exceptionThrown')errors.push(message.params.exceptionDetails.exception?.description||message.params.exceptionDetails.text);});
    const command=(method,params={})=>new Promise((resolve,reject)=>{const id=next++;const timer=setTimeout(()=>{pending.delete(id);reject(Error('超时：'+method));},10000);pending.set(id,{resolve:r=>{clearTimeout(timer);resolve(r);},reject:e=>{clearTimeout(timer);reject(e);}});socket.send(JSON.stringify({id,method,params}));});
    async function evaluate(expression){const r=await command('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(r.exceptionDetails)throw Error(r.exceptionDetails.exception?.description||r.exceptionDetails.text);return r.result?.value;}
    async function check(expression,label){if(!await evaluate(expression))throw Error(label);checks[label]=true;}
    async function click(selector){await evaluate(`document.querySelector(${JSON.stringify(selector)}).click()`);await pause(450);}
    async function shot(name){const value=await command('Page.captureScreenshot',{format:'png'});fs.writeFileSync(path.join(output,name+'.png'),Buffer.from(value.data,'base64'));}
    await command('Runtime.enable');await command('Page.enable');await pause(500);
    if(process.env.XIYUAN_INSTALLER_FAILURES==='1'){
      // 通过同一原生消息入口检查已有目录保护与取消，不写正式注册分支。
      await evaluate("window.__events=[];chrome.webview.addEventListener('message',e=>window.__events.push(e.data));");
      const occupied=path.join(output,'已有目录');fs.mkdirSync(occupied);fs.writeFileSync(path.join(occupied,'用户文件.txt'),'保留内容');fs.mkdirSync(path.join(occupied,'Xiyuan.exe'));
      async function expectError(){for(let i=0;i<100;i++){if(await evaluate("window.__events.some(e=>e.type==='error')"))return;await pause(50);}throw Error('未返回预期安装失败');}
      await evaluate(`chrome.webview.postMessage({action:'install',path:${JSON.stringify(occupied)},desktop:false,menu:false,associate:false})`);await expectError();
      if(fs.readFileSync(path.join(occupied,'用户文件.txt'),'utf8')!=='保留内容')throw Error('用户文件被修改');checks['目录文件冲突保护用户文件']=true;
      await evaluate('window.__events=[]');const cancelled=path.join(output,'取消安装');
      await evaluate(`chrome.webview.postMessage({action:'install',path:${JSON.stringify(cancelled)},desktop:false,menu:false,associate:false});chrome.webview.postMessage({action:'close'});`);await expectError();
      if(fs.existsSync(cancelled)||fs.readdirSync(output).some(n=>n.startsWith('取消安装.install-')))throw Error('取消后残留安装目录');checks['取消安装清理本次暂存']=true;
      await evaluate('location.reload()');await pause(600);
    }
    await check('document.querySelector(".xy-logo").naturalWidth===256','用户图标实际加载');
    await check('document.querySelector(".xy-layout").getBoundingClientRect().width===878','布局宽度对齐原型');
    await check('getComputedStyle(document.querySelector(".xy-layout")).gridTemplateColumns.startsWith("137px")','侧栏宽度对齐原型');
    await check('document.querySelectorAll(".xy-features li").length===5','五项功能以简短文字介绍');
    await check('document.querySelectorAll(".xy-screenshots img").length===2&&[...document.querySelectorAll(".xy-screenshots img")].every(image=>image.complete&&image.naturalWidth===648)','两张真实静态截图已加载');
    await check('!document.querySelector("[data-film]")&&document.getAnimations().length===0','功能介绍没有轮播或演示动画');
    await check('document.querySelector(".xy-features").getBoundingClientRect().width>document.querySelector(".xy-screenshots").getBoundingClientRect().width','文字介绍为主图片为辅');
    await check('document.querySelector(".xy-footer").getBoundingClientRect().bottom<=innerHeight&&document.querySelector(".xy-content").scrollHeight<=document.querySelector(".xy-content").clientHeight','介绍和底部操作完整显示');
    await shot('欢迎');
    await command('Emulation.setEmulatedMedia',{features:[{name:'prefers-reduced-motion',value:'reduce'}]});
    await check('document.querySelector(".xy-screenshots").getBoundingClientRect().height>0&&document.querySelectorAll(".xy-features li").length===5','减少动态效果时功能介绍保持可见');
    await command('Emulation.setEmulatedMedia',{features:[]});
    await evaluate('document.querySelector("[data-action=next]").click()');await pause(80);
    await check('document.querySelectorAll(".xy-page").length===2&&document.querySelector(".xy-leaving").inert','实际执行融合转场');await shot('融合转场');await pause(380);
    await check('document.querySelector("#xy-path").value===String.raw`C:\\Program Files\\XiYuan`','默认目录为系统ProgramFiles下的XiYuan');
    await shot('安装位置');
    // 只输入盘符也允许继续，失焦后自动补全产品文件夹。
    await evaluate('document.querySelector("#xy-path").value="D:"+String.fromCharCode(92);document.querySelector("#xy-path").dispatchEvent(new Event("input",{bubbles:true}));document.querySelector("#xy-path").dispatchEvent(new FocusEvent("focusout",{bubbles:true}))');
    await check('document.querySelector("#xy-path").value===String.raw`D:\\XiYuan`&&!document.querySelector("[data-action=next]").disabled','输入盘符自动补全XiYuan文件夹');
    await evaluate('document.querySelector("#xy-path").value="badpath";document.querySelector("#xy-path").dispatchEvent(new Event("input",{bubbles:true}))');
    await check('document.querySelector("[data-action=next]").disabled','无效目录被拦截');
    const destination=path.resolve(process.env.XIYUAN_INSTALLER_DESTINATION||path.join(output,'安装目录'));
    await evaluate(`document.querySelector('#xy-path').value=${JSON.stringify(destination)};document.querySelector('#xy-path').dispatchEvent(new Event('input',{bubbles:true}));`);
    await click('[data-action=next]');
    await check('!document.querySelector("[data-option=associate]").checked','文件关联默认不勾选');
    await evaluate(`for(const key of ['desktop','menu']){const e=document.querySelector('[data-option='+key+']');e.checked=false;e.dispatchEvent(new Event('change',{bubbles:true}));}const e=document.querySelector('[data-option=associate]');e.checked=true;e.dispatchEvent(new Event('change',{bubbles:true}));`);
    await shot('选项');await click('[data-action=next]');
    await check('document.querySelector(".xy-mode-notes")===null&&document.querySelectorAll(".xy-features li").length===5','安装期间保留静态功能说明');
    await check('document.querySelector(".xy-content").scrollHeight<=document.querySelector(".xy-content").clientHeight','安装进度和功能介绍完整显示');
    await shot('安装中');
    for(let i=0;i<1500;i++){if(await evaluate('document.querySelector("[data-action=finish]")!==null'))break;await pause(100);}
    await check('document.querySelector(".xy-finishmark").textContent.includes("安装完成")','真实安装完成');
    await shot('完成');
    if(!fs.existsSync(path.join(destination,'Xiyuan.exe'))||!fs.existsSync(path.join(destination,'.xiyuan-install.json')))throw Error('安装文件缺失');
    checks['文件校验解压并发布安装目录']=true;
    const installed=JSON.parse(fs.readFileSync(path.join(destination,'.xiyuan-install.json'),'utf8'));
    if(installed.scope!=='machine')throw Error('安装范围未使用系统安装');checks['系统安装范围正确']=true;
    const registry=spawn('reg.exe',['query',`HKCU\\Software\\Xiyuan\\InstallerTests\\${processHandle.pid}\\Software\\Classes\\Xiyuan.Safetensors\\shell\\open\\command`],{windowsHide:true});let registryOutput='';registry.stdout.on('data',d=>registryOutput+=d.toString());await new Promise(resolve=>registry.on('exit',resolve));
    if(!registryOutput.includes('--open-file'))throw Error('关联命令未写入');checks['隔离环境验证文件关联命令']=true;
    await click('[data-action=finish]');
    if(errors.length)throw Error(errors.join('\n'));
    fs.writeFileSync(path.join(output,'安装器界面验收.json'),JSON.stringify({pass:true,checks},null,2));console.log(JSON.stringify(checks));
  }finally{socket?.close();if(processHandle.exitCode===null)processHandle.kill();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
