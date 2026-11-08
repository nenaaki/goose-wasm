import { load } from './host.mjs';

const h = await load();
const x = h.x;
x.goose_init();
console.log(x.sum_squares(10n).toString());
console.log(x.sum_squares(0n).toString());
console.log(`${x.contained(1n)} ${x.contained(-1n)}`);
console.log(`${x.ask(4n)} ${x.ask(7n)}`);
console.log(x.sum_squares(5n).toString());
console.log(x.stop().toString());
