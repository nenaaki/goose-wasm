import { load } from './host.mjs';

const h = await load();
const x = h.x;
x.goose_init();
console.log(x.sum_squares(10n).toString());
x.shout(3);
x.shout(0);
console.log(x.at(2n));
console.log(x.at(4n));
