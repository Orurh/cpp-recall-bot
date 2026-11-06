// Real signed local HTTP requests; no production identities or external APIs.
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';

export async function checkSystemDesignHttp({api, origin, owner}) {
  const json = async (action, body={}) => {
    const response=await api(`system-design/${action}`,owner,body,0,5000);
    assert.equal(response.status,200,await response.clone().text()); return response.json();
  };
  const ordinary=owner+500;
  const regular=await (await api('bootstrap',ordinary)).json();
  assert.deepEqual(regular.directions,[{id:'cpp',title:'C++ Engineering'}]);
  const own=await (await api('bootstrap',owner)).json();
  assert.deepEqual(own.directions.map(item=>item.id),['cpp','system_design']);
  for(const action of ['bootstrap','module','complete-module','start','current','session','save','hint','advance','self-review']) {
    assert.equal((await api(`system-design/${action}`,ordinary,{user_id:owner,owner_id:owner})).status,403,action);
    assert.equal((await fetch(new URL(`api/v1/system-design/${action}`,origin),{method:'POST',body:'{}'})).status,401,action);
  }
  assert.equal((await fetch(new URL('api/v1/system-design/bootstrap',origin),{method:'POST',
    headers:{Authorization:'tma forged-initData'},body:'{}'})).status,401);
  for(const pathname of ['system-design','system-design/catalog.json','content/system_design/catalog.json'])
    assert.equal((await fetch(new URL(pathname,origin))).status,404,pathname);
  const bundle=await (await fetch(new URL('system-design.js',origin))).text();
  for(const secretMaterial of ['73 млрд байт','500 000 новых ссылок','Как проходить System Design Interview'])
    assert(!bundle.includes(secretMaterial),'protected curriculum shipped in public JS');
  const intro=await json('module'); assert(intro.sections.length>=6);
  await json('complete-module',{acknowledged:true});
  const session=await json('start',{case_id:'url_shortener',request_id:randomUUID()});
  const saved=await json('save',{session_id:session.session_id,version:session.version,stage_id:session.stage.id,
    request_id:randomUUID(),text:'Свой подход HTTP: требования и ограничения.'});
  assert.equal((await json('current')).session.stage.draft,saved.stage.draft);
  assert.equal(saved.review,undefined);
  assert.equal((await api('system-design/session',ordinary,{session_id:session.session_id})).status,403);
  console.log('SD-01 HTTP PASS: owner-only directions, every endpoint denied for regular/unsigned user, protected material absent from public bundle');
}
