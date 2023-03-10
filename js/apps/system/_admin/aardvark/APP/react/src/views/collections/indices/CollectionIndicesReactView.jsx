import React from "react";
import { ChakraCustomProvider } from "../../../theme/ChakraCustomProvider";
import { CollectionIndicesTabWrap } from "./CollectionIndicesTabWrap";

const CollectionIndicesReactView = ({ collectionName, collection }) => {
  return (
    <ChakraCustomProvider>
      <CollectionIndicesTabWrap
        collection={collection}
        collectionName={collectionName}
      />
    </ChakraCustomProvider>
  );
};
window.CollectionIndicesReactView = CollectionIndicesReactView;
