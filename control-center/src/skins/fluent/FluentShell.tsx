import type { ShellProps } from "../../app/shell";
import { ClassicShell } from "../classic/ClassicShell";

export function FluentShell(props: ShellProps) {
  return <ClassicShell {...props} />;
}
