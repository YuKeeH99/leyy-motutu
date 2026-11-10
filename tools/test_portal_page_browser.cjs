/* Real embedded-page browser checks; no device, radio or external HTTP.
 * Run: node --test tools/test_portal_page_browser.cjs
 * Optional: PLAYWRIGHT_MODULE_PATH, CHROME_PATH, PORTAL_SCREENSHOT_DIR.
 * A resized / synthetic visual viewport is not a real phone keyboard test.
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { test, before, after } = require('node:test');

let playwright;
for (const candidate of [process.env.PLAYWRIGHT_MODULE_PATH, 'playwright',
  path.join(os.homedir(), '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright')].filter(Boolean)) {
  try { playwright = require(candidate); break; } catch (error) { if (error.code !== 'MODULE_NOT_FOUND') throw error; }
}
if (!playwright) throw new Error('Install playwright or set PLAYWRIGHT_MODULE_PATH.');
const chrome = [process.env.CHROME_PATH,
  process.env.PROGRAMFILES && path.join(process.env.PROGRAMFILES, 'Google/Chrome/Application/chrome.exe'),
  '/usr/bin/google-chrome', '/usr/bin/chromium'].filter(Boolean).find(fs.existsSync);
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'src/middleware/netcfg_portal/netcfg_portal_page.c'), 'utf8');
const declaration = source.slice(source.indexOf('const char netcfg_portal_page_html[] ='));
const html = [...declaration.matchAll(/"(?:[^"\\]|\\.)*"/g)].map(x => JSON.parse(x[0])).join('');
const screenshots = process.env.PORTAL_SCREENSHOT_DIR || path.join(root, 'tmp/portal-page');
const networks = Array.from({length:24}, (_, i) => ({ssid:`Test WiFi ${i+1}`,rssi:-38-i*2,sec:1}));
let browser;
before(async()=>{fs.mkdirSync(screenshots,{recursive:true});browser=await playwright.chromium.launch({headless:true,...(chrome?{executablePath:chrome}:{})});});
after(async()=>{if(browser)await browser.close();});

async function openPage(t, {width=320,height=568,aps=networks,connect='ok',fakeViewport=false,withoutViewport=false}={}) {
  const context=await browser.newContext({viewport:{width,height},isMobile:true,hasTouch:true,deviceScaleFactor:1});
  t.after(()=>context.close());
  if(fakeViewport)await context.addInitScript(()=>{
    const view = new EventTarget();
    view.height=window.innerHeight;view.width=window.innerWidth;view.offsetTop=0;view.scale=1;
    Object.defineProperty(window,'visualViewport',{value:view});
  });
  if(withoutViewport)await context.addInitScript(()=>{Object.defineProperty(window,'visualViewport',{value:undefined});});
  const page=await context.newPage();
  const requests=[],errors=[];
  page.on('pageerror',error=>errors.push(error.message));
  await page.route('**/*',async route=>{
    const pathname=new URL(route.request().url()).pathname;
    if(pathname==='/api/connect'){
      requests.push(new URLSearchParams(route.request().postData()));
      if(connect==='abort')return route.abort('failed');
      if(connect==='bad-json')return route.fulfill({status:200,contentType:'application/json',body:'{'});
    }
    const body=pathname==='/api/aps'?JSON.stringify({aps}):pathname==='/api/info'?JSON.stringify({ssid:'ARCS-TEST'}):pathname==='/api/connect'?JSON.stringify(connect==='reject'?{ok:false,err:'密码格式不正确，请重新输入'}:connect==='long-reject'?{ok:false,err:'连接暂未受理，请检查网络名称和密码。'.repeat(24)}:{ok:true}):html;
    await route.fulfill({status:200,contentType:pathname.startsWith('/api/')?'application/json':'text/html; charset=utf-8',body});
  });
  await page.goto('http://portal.test/');
  await page.waitForFunction(()=>document.getElementById('apn').textContent==='ARCS-TEST' && !document.getElementById('ls').textContent.includes('正在读取'));
  return {page,requests,errors};
}

async function visible(page, selector, label) {
  const b=await page.locator(selector).evaluate(e=>{
    const r=e.getBoundingClientRect(),v=window.visualViewport;
    return {top:r.top,bottom:r.bottom,left:r.left,right:r.right,viewTop:v?v.offsetTop:0,viewBottom:v?v.offsetTop+v.height:innerHeight,width:innerWidth,height:r.height,display:getComputedStyle(e).display};
  });
  assert.ok(b.display!=='none' && b.height>0 && b.top>=b.viewTop-1 && b.bottom<=b.viewBottom+1 && b.left>=0 && b.right<=b.width+1,`${label}: ${JSON.stringify(b)}`);
}

async function resizeViewport(page, size) {
  await page.setViewportSize(size);
  // setViewportSize acknowledges browser metrics, not delivery of resize to JS.
  // Wait for fit()'s observable result, then assert the button independently.
  // A missing/broken resize handler still fails this bounded wait.
  await page.waitForFunction(()=>{
    const v=window.visualViewport,r=document.documentElement;
    const height=v?v.height:innerHeight,top=v?v.offsetTop:0;
    const box=document.getElementById('viewport').getBoundingClientRect();
    return Math.abs(parseFloat(r.style.getPropertyValue('--view-height'))-height)<1 &&
      Math.abs(parseFloat(r.style.getPropertyValue('--view-top'))-top)<1 &&
      Math.abs(box.height-height)<1 && Math.abs(box.top-top)<1;
  },null,{timeout:2000});
}

for(const [width,height] of [[320,568],[360,640],[390,844],[568,320]]) {
  test(`${width}x${height}: 24 APs keep connect visible without opening a keyboard`,async t=>{
    const {page,errors}=await openPage(t,{width,height});
    await visible(page,'#go','initial button');
    await page.screenshot({path:path.join(screenshots,`${width}x${height}-initial.png`)});
    assert.match(await page.locator('body').innerText(),/2\.4\s*GHz/);
    const list=await page.locator('#ls').boundingBox();
    await page.mouse.move(list.x+list.width/2,list.y+Math.min(list.height/2,50));
    await page.mouse.wheel(0,500);
    await page.waitForFunction(()=>document.getElementById('ls').scrollTop>0);
    await visible(page,'#go','button after scrolling networks');
    await page.locator('#ls li').last().tap();
    assert.equal(await page.locator('#ssid').inputValue(),'Test WiFi 24');
    assert.notEqual(await page.evaluate(()=>document.activeElement.id),'pwd','selection must not require opening a keyboard');
    await visible(page,'#go','button after selecting last network');
    await page.locator('#pwd').fill('test-only');
    await page.locator('#sp').check();
    assert.equal(await page.locator('#pwd').getAttribute('type'),'text');
    await page.locator('#sp').uncheck();
    assert.equal(await page.locator('#pwd').getAttribute('type'),'password');
    await visible(page,'#go','button after reaching form controls');
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth),width);
    await page.screenshot({path:path.join(screenshots,`${width}x${height}-form.png`)});
    assert.deepEqual(errors,[]);
  });
}

test('empty list permits manual 2.4GHz credentials and encodes the request',async t=>{
  const {page,requests}=await openPage(t,{aps:[]});
  await visible(page,'#go','empty-list button');
  assert.match(await page.locator('#ls').innerText(),/手动输入/);
  await page.locator('#ssid').fill('Test & 中 文');
  await page.locator('#pwd').fill('test+&=%');
  await page.locator('#go').tap();
  await page.waitForFunction(()=>document.getElementById('go').textContent==='已提交');
  assert.equal(requests.length,1);
  assert.equal(requests[0].get('ssid'),'Test & 中 文');
  assert.equal(requests[0].get('pwd'),'test+&=%');
  assert.equal(await page.locator('#go').isDisabled(),true);
  await visible(page,'#m','accepted message');
});

test('missing SSID and server error remain visible beside the connect action',async t=>{
  const {page,requests}=await openPage(t,{connect:'reject'});
  await page.locator('#go').tap();
  assert.match(await page.locator('#m').innerText(),/网络名称/);
  await visible(page,'#m','SSID error');
  assert.equal(requests.length,0);
  await page.locator('#ssid').fill('Test');
  await page.locator('#go').tap();
  await page.waitForFunction(()=>document.getElementById('m').textContent.includes('密码格式'));
  await visible(page,'#m','server error');
  await visible(page,'#go','retry button');
  assert.equal(await page.locator('#go').isDisabled(),false);
  await page.screenshot({path:path.join(screenshots,'320x568-error.png')});
});

for(const connect of ['abort','bad-json'])test(`${connect}: an unconfirmed response is not reported as submitted`,async t=>{
  const {page}=await openPage(t,{connect});
  await page.locator('#ssid').fill('Test');
  await page.locator('#go').tap();
  await page.waitForFunction(()=>document.getElementById('m').textContent.length>0);
  assert.match(await page.locator('#m').innerText(),/未确认/);
  assert.doesNotMatch(await page.locator('#m').innerText(),/已提交/);
  assert.equal(await page.locator('#go').isDisabled(),false);
  await visible(page,'#m','uncertain result');
  await visible(page,'#go','uncertain retry');
});

test('layout resize with a focused password keeps action and error reachable',async t=>{
  const {page}=await openPage(t,{width:360,height:640,connect:'reject'});
  await page.locator('#ssid').fill('Test');
  await page.locator('#pwd').fill('test-only');
  await resizeViewport(page,{width:360,height:300});
  await visible(page,'#go','reduced-height button');
  await page.locator('#go').tap();
  await page.waitForFunction(()=>document.getElementById('m').textContent.includes('密码格式'));
  await visible(page,'#m','reduced-height error');
  await visible(page,'#go','reduced-height retry');
  await page.screenshot({path:path.join(screenshots,'360x300-resized-error.png')});
  await resizeViewport(page,{width:360,height:640});
  await visible(page,'#go','restored-height button');
});

test('visual viewport shrink and offset move the action above the obscured area',async t=>{
  const {page}=await openPage(t,{width:360,height:640,fakeViewport:true});
  await page.locator('#ssid').fill('Test');
  await page.locator('#pwd').fill('test-only');
  await page.evaluate(()=>{visualViewport.height=280;visualViewport.offsetTop=60;visualViewport.dispatchEvent(new Event('resize'));visualViewport.dispatchEvent(new Event('scroll'));});
  await visible(page,'#go','visual viewport button');
  await page.screenshot({path:path.join(screenshots,'360x640-visual-viewport.png')});
  await page.evaluate(()=>{visualViewport.height=640;visualViewport.offsetTop=0;visualViewport.dispatchEvent(new Event('resize'));});
  await visible(page,'#go','visual viewport restored');
});

test('older browsers without visualViewport retain visible action on resize',async t=>{
  const {page}=await openPage(t,{withoutViewport:true});
  await visible(page,'#go','fallback initial button');
  await resizeViewport(page,{width:320,height:280});
  await visible(page,'#go','fallback resized button');
});

test('a long error fits a small visual viewport and repeated scroll events do not re-scroll fields',async t=>{
  const {page}=await openPage(t,{width:320,height:568,fakeViewport:true,connect:'long-reject'});
  await page.locator('#ssid').fill('Test');
  await page.locator('#pwd').fill('test-only');
  await page.evaluate(()=>{
    const original=Element.prototype.scrollIntoView;
    window.fieldScrolls=0;
    Element.prototype.scrollIntoView=function(...args){window.fieldScrolls++;return original.apply(this,args);};
    visualViewport.height=180;visualViewport.offsetTop=40;visualViewport.dispatchEvent(new Event('resize'));
  });
  await visible(page,'#go','small visual viewport button');
  await page.locator('#go').tap();
  await page.waitForFunction(()=>document.getElementById('m').textContent.includes('暂未受理'));
  await visible(page,'#m','bounded long error');
  await visible(page,'#go','small visual viewport retry');
  assert.equal(await page.locator('#go').isDisabled(),false);
  assert.ok(await page.locator('#m').evaluate(e=>e.scrollHeight>e.clientHeight),'long feedback must remain scrollable');
  await page.screenshot({path:path.join(screenshots,'320x568-small-visual-long-error.png')});
  await page.locator('#pwd').focus();
  const before=await page.evaluate(()=>window.fieldScrolls);
  await page.evaluate(()=>{for(let i=0;i<20;i++)visualViewport.dispatchEvent(new Event('scroll'));});
  assert.equal(await page.evaluate(()=>window.fieldScrolls),before,'unchanged viewport scroll must not re-scroll focused input');
  await page.evaluate(()=>{visualViewport.offsetTop=50;visualViewport.dispatchEvent(new Event('scroll'));});
  assert.equal(await page.evaluate(()=>window.fieldScrolls),before,'offset-only updates must not feed back into field scrolling');
  await visible(page,'#go','offset-only update');
});

test('network names are rendered as text and refresh preserves manual fields',async t=>{
  const {page,errors}=await openPage(t,{aps:[{ssid:'<img src=x onerror=alert(1)>',rssi:-40,sec:0}]});
  assert.equal(await page.locator('#ls img').count(),0);
  await page.locator('#ssid').fill('Manual');
  await page.locator('#pwd').fill('test-only');
  await page.locator('#rf').tap();
  await page.waitForFunction(()=>document.querySelectorAll('#ls li').length===1);
  assert.equal(await page.locator('#ssid').inputValue(),'Manual');
  assert.equal(await page.locator('#pwd').inputValue(),'test-only');
  assert.deepEqual(errors,[]);
});
