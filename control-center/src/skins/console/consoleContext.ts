import { createContext, useContext } from "react";
import type { ShellProps } from "../../app/shell";
import type { ExecutionModel } from "../../features/execution/useExecutionModel";

export interface ConsoleContextValue {
  shell: ShellProps;
  execution: ExecutionModel;
}

export const ConsoleContext = createContext<ConsoleContextValue | null>(null);

export function useConsole(): ConsoleContextValue {
  const value = useContext(ConsoleContext);
  if (!value) throw new Error("useConsole must be used inside the Console shell");
  return value;
}
