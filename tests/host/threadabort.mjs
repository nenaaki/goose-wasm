import { load } from './host.mjs';

const h = await load();
const x = h.x;
x.goose_init();
console.log(x.run(1n).toString());
console.log(x.run(5n).toString());
console.log('not reached');
