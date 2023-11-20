// 1. Import `extendTheme`
import {
  extendTheme,
  theme as defaultTheme,
  TooltipProps
} from "@chakra-ui/react";
import { cssVar, mode, StyleFunctionProps } from "@chakra-ui/theme-tools";

const $tooltipBg = cssVar("tooltip-bg");

// 2. Call `extendTheme` and pass your custom values
export const theme = extendTheme({
  fonts: {
    heading: "Inter, sans-serif",
    body: "Inter, sans-serif"
  },
  colors: {
    green: {
      50: "#f5faeb",
      100: "#e9f4d3",
      200: "#d5e9ad",
      300: "#b8da7c",
      400: "#9dc853",
      500: "#7ead35",
      600: "#608726",
      700: "#4b6922",
      800: "#3e5420",
      900: "#35481f",
      950: "#1a270c"
    },
    gray: {
      50: "#f8f8f8",
      100: "#f0f0f0",
      200: "#e5e5e5",
      300: "#d1d1d1",
      400: "#b4b4b4",
      500: "#9a9a9a",
      600: "#818181",
      700: "#6a6a6a",
      800: "#5a5a5a",
      900: "#4e4e4e",
      950: "#282828"
    }
  },
  components: {
    Switch: {
      defaultProps: {
        colorScheme: "green"
      },
      baseStyle: {
        track: {
          _checked: {
            bg: "green.600"
          }
        }
      }
    },
    Button: {
      variants: {
        solid: (props: StyleFunctionProps) => {
          if (props.colorScheme === "green") {
            return {
              bg: "green.600",
              color: "white",
              _hover: {
                bg: "green.700"
              },
              _active: {
                bg: "green.700"
              }
            };
          }
          return defaultTheme.components.Button.variants.solid(props);
        }
      }
    },
    Tooltip: {
      baseStyle: (props: TooltipProps) => {
        const bg = mode("gray.900", "gray.300")(props);
        return {
          [$tooltipBg.variable]: `colors.${bg}`
        };
      }
    },
    Popover: {
      baseStyle: {
        popper: {
          zIndex: 500
        }
      }
    },
    Modal: {
      sizes: {
        max: {
          dialog: {
            maxW: "calc(100vw - 200px)",
            minH: "100vh",
            my: 0
          }
        }
      }
    }
  },
  styles: {
    global: {
      body: {
        color: "gray.950"
      }
    }
  }
});
