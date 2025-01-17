import hotkeys from "hotkeys-js";
import React from "react";
import { useQueryContext } from "./QueryContextProvider";

export const QueryKeyboardShortcutProvider = ({
  children
}: {
  children: React.ReactNode;
}) => {
  const {
    onExecute,
    onExplain,
    onOpenSpotlight,
    queryValue,
    queryBindParams,
    queryOptions,
    disabledRules
  } = useQueryContext();
  React.useEffect(() => {
    hotkeys.filter = function (event) {
      var target = event.target;
      var tagName = (target as any)?.tagName;
      return tagName === "INPUT" || tagName === "TEXTAREA";
    };

    hotkeys("ctrl+enter,command+enter", () => {
      onExecute({ queryValue, queryBindParams, queryOptions, disabledRules });
    });

    hotkeys("ctrl+space", onOpenSpotlight);

    hotkeys("ctrl+shift+enter,command+shift+enter", () => {
      onExplain({
        queryValue,
        queryBindParams,
        queryOptions,
        disabledRules
      });
    });
    return () => {
      hotkeys.unbind();
    };
  }, [
    onExecute,
    onExplain,
    queryValue,
    queryOptions,
    queryBindParams,
    disabledRules,
    onOpenSpotlight
  ]);
  return <>{children}</>;
};
