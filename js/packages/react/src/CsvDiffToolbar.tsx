/**
 * The toolbar of spec 8.4: the "show unchanged rows" checkbox and the layout
 * picker.
 *
 * It is controlled rather than stateful, and that is the whole design. Toggling
 * the filter rebuilds the report index in the engine, so the state has to live
 * where the source is built; a toolbar that owned it would have to reach into the
 * table to rebuild one, which is the coupling this package exists to avoid.
 *
 * It renders no summary of its own. `handle.summary()` already carries what a
 * header bar wants, including `identical`, the findings counts with the `enabled`
 * flag that distinguishes "nothing found" from "not looked at", and
 * `targetHeader.namesOnly`, and rendering those is an application's decision
 * about its own words.
 */

import { classNames, ROW_LAYOUTS, type RowLayout } from '@ibhatech/csvdiff-view';

export interface CsvDiffToolbarProps {
  changesOnly: boolean;
  onChangesOnlyChange: (value: boolean) => void;
  layout?: RowLayout;
  onLayoutChange?: (layout: RowLayout) => void;
  classPrefix?: string;
  /** Report rows under the current filter, for the count beside the checkbox. */
  rowCount?: number;
}

export function CsvDiffToolbar(props: CsvDiffToolbarProps) {
  const c = classNames(props.classPrefix);
  const id = 'ibha-csvd-show-unchanged';

  return (
    <div className={`${c.report}-toolbar`} role="toolbar" aria-label="diff view options">
      <label htmlFor={id}>
        <input
          id={id}
          type="checkbox"
          checked={!props.changesOnly}
          onChange={(e) => props.onChangesOnlyChange(!e.target.checked)}
        />
        {' show unchanged rows'}
      </label>
      {props.rowCount !== undefined ? <span>{` ${props.rowCount} rows`}</span> : null}
      {props.onLayoutChange ? (
        <label>
          {' layout '}
          <select
            value={props.layout ?? 'inline'}
            onChange={(e) => props.onLayoutChange?.(e.target.value as RowLayout)}
          >
            {ROW_LAYOUTS.map((l) => (
              <option key={l} value={l}>
                {l}
              </option>
            ))}
          </select>
        </label>
      ) : null}
    </div>
  );
}
