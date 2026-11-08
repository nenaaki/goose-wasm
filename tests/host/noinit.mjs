import { load } from './host.mjs';

const h = await load();
console.log('before');
console.log(h.x.answer());
