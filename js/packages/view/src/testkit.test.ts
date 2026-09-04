import { describe, expect, it } from 'vitest';

import { htmlSafetyViolation, XSS_COLUMN_NAMES, XSS_VALUES } from './testkit.ts';

/**
 * A safety checker nothing tests is worth nothing: `return null` passes every
 * test written against it, and every renderer it is pointed at. So this file
 * tests the checker itself, in both directions.
 */
describe('htmlSafetyViolation', () => {
  it('accepts the markup the emitter and the view actually write', () => {
    const html =
      '<div class="ibha-csvd-report" data-schema-version="1"><table class="ibha-csvd-table">\n' +
      '<thead><tr><th class="ibha-csvd-th ibha-csvd-num">row</th></tr></thead>\n<tbody>\n' +
      '<tr class="ibha-csvd-row ibha-csvd-modified"><td class="ibha-csvd-num">4</td>' +
      '<td class="ibha-csvd-cell ibha-csvd-changed ibha-csvd-finding" data-finding="tooLong">' +
      '<span class="ibha-csvd-old">a<del class="ibha-csvd-del">b</del></span>' +
      '<span class="ibha-csvd-new">a<ins class="ibha-csvd-ins">c</ins></span></td></tr>\n' +
      '</tbody></table></div>\n';
    expect(htmlSafetyViolation(html)).toBeNull();
  });

  it('accepts escaped cell content, including every entity a renderer emits', () => {
    expect(htmlSafetyViolation('<td>&lt;script&gt;alert(1)&lt;/script&gt;</td>')).toBeNull();
    expect(htmlSafetyViolation('<td>&amp;amp;</td>')).toBeNull();
    // React writes &#x27; where the C emitter writes &#39;. Both are the same
    // character and neither can open a tag.
    expect(htmlSafetyViolation('<td>it&#x27;s</td>')).toBeNull();
    expect(htmlSafetyViolation('<td>it&#39;s</td>')).toBeNull();
    expect(htmlSafetyViolation('<td aria-label="a &amp; b">x</td>')).toBeNull();
    expect(htmlSafetyViolation('<td data-column="tax &quot;rate&quot;">x</td>')).toBeNull();
  });

  it('rejects every value in the corpus when it is not escaped', () => {
    for (const value of XSS_VALUES) {
      const asText = `<td class="c">${value}</td>`;
      const asAttribute = `<td class="c" data-column="${value}">x</td>`;
      const bad = htmlSafetyViolation(asText) ?? htmlSafetyViolation(asAttribute);
      expect(bad, `unescaped ${JSON.stringify(value)} was accepted`).not.toBeNull();
    }
  });

  it('rejects the vectors a grep for <script> would let through', () => {
    // The reason the invariant is stated positively. Each of these is live markup
    // and none of them contains the string "script".
    expect(htmlSafetyViolation('<td><img src=x onerror=alert(1)></td>')).toContain('img');
    expect(htmlSafetyViolation('<td><svg/onload=alert(1)></td>')).toContain('svg');
    expect(htmlSafetyViolation('<td><iframe src="data:text/html,x"></iframe></td>')).toContain(
      'iframe',
    );
  });

  it('rejects an attribute breakout, which carries no angle bracket at all', () => {
    // `" onload="x` closes the attribute it is inside and opens an event handler.
    // The result is well formed markup, which is exactly why checking tags and
    // quoting is not enough and the attribute names have to be checked too.
    expect(htmlSafetyViolation('<td class="c" onload="alert(1)">x</td>')).toContain('onload');
    expect(htmlSafetyViolation('<td data-column="a"onload="alert(1)">x</td>')).toContain('onload');
    expect(htmlSafetyViolation('<td class="c" onclick=alert(1)>x</td>')).toContain('onclick');
    // An href is not executable by itself, but `javascript:` in one is, so no
    // element the view writes is allowed to carry one.
    expect(htmlSafetyViolation('<span href="javascript:alert(1)">x</span>')).toContain('href');
    expect(htmlSafetyViolation('<td class="a"><script>alert(1)</script></td>')).toContain('script');
  });

  it('rejects an unquoted attribute value, which is a breakout waiting to happen', () => {
    expect(htmlSafetyViolation('<td data-column=a b>x</td>')).toContain('unquoted');
  });

  it('rejects an unterminated tag and an unterminated attribute', () => {
    expect(htmlSafetyViolation('<td class="c">x')).toBeNull();
    expect(htmlSafetyViolation('<td class="c')).toContain('unterminated');
    expect(htmlSafetyViolation('<td class="c" ')).toContain('unterminated tag');
  });

  it('rejects a bare ampersand in text, which is how double escaping hides', () => {
    expect(htmlSafetyViolation('<td>a & b</td>')).toContain("raw '&'");
    expect(htmlSafetyViolation('<td>&notanentity;</td>')).toContain("raw '&'");
  });

  it('has a corpus with something in it', () => {
    expect(XSS_VALUES.length).toBeGreaterThan(8);
    expect(XSS_COLUMN_NAMES.length).toBeGreaterThan(2);
  });
});
