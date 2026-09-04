import type { ShellProps } from "../../app/shell";
import { ClassicShell } from "../classic/ClassicShell";

export function CupertinoShell(props: ShellProps) {
  return <ClassicShell {...props} />;
}
