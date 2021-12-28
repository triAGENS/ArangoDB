/*
 @licstart  The following is the entire license notice for the JavaScript code in this file.

 The MIT License (MIT)

 Copyright (C) 1997-2020 by Dimitri van Heesch

 Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 and associated documentation files (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge, publish, distribute,
 sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all copies or
 substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 @licend  The above is the entire license notice for the JavaScript code in this file
*/
var NAVTREE =
[
  [ "Boost.Hana", "index.html", [
    [ "User Manual", "index.html", [
      [ "Description", "index.html#tutorial-description", null ],
      [ "Prerequisites and installation", "index.html#tutorial-installation", [
        [ "Note for CMake users", "index.html#tutorial-installation-cmake", null ],
        [ "Compiler requirements", "index.html#tutorial-installation-requirements", null ]
      ] ],
      [ "Support", "index.html#tutorial-support", null ],
      [ "Introduction", "index.html#tutorial-introduction", [
        [ "C++ computational quadrants", "index.html#tutorial-introduction-quadrants", null ],
        [ "What is this library about?", "index.html#tutorial-quadrants-about", null ]
      ] ],
      [ "Quick start", "index.html#tutorial-quickstart", [
        [ "A real world example", "index.html#tutorial-quickstart-any", null ]
      ] ],
      [ "Cheatsheet", "index.html#tutorial-cheatsheet", null ],
      [ "Assertions", "index.html#tutorial-assert", null ],
      [ "Compile-time numbers", "index.html#tutorial-integral", [
        [ "Compile-time arithmetic", "index.html#tutorial-integral-arithmetic", null ],
        [ "Example: Euclidean distance", "index.html#tutorial-integral-distance", null ],
        [ "Compile-time branching", "index.html#tutorial-integral-branching", null ],
        [ "Why stop here?", "index.html#tutorial-integral-more", null ]
      ] ],
      [ "Type computations", "index.html#tutorial-type", [
        [ "Types as objects", "index.html#tutorial-type-objects", null ],
        [ "Benefits of this representation", "index.html#tutorial-type-benefits", null ],
        [ "Working with this representation", "index.html#tutorial-type-working", null ],
        [ "The generic lifting process", "index.html#tutorial-type-lifting", null ]
      ] ],
      [ "Introspection", "index.html#tutorial-introspection", [
        [ "Checking expression validity", "index.html#tutorial-introspection-is_valid", [
          [ "Remarks", "index.html#autotoc_md419", null ],
          [ "Non-static members", "index.html#tutorial-introspection-is_valid-non_static", null ],
          [ "Static members", "index.html#tutorial-introspection-is_valid-static", null ],
          [ "Nested type names", "index.html#tutorial-introspection-is_valid-nested-typename", null ],
          [ "Nested templates", "index.html#tutorial-introspection-is_valid-nested-template", null ],
          [ "Template specializations", "index.html#tutorial-introspection-is_valid-template", null ]
        ] ],
        [ "Taking control of SFINAE", "index.html#tutorial-introspection-sfinae", null ],
        [ "Introspecting user-defined types", "index.html#tutorial-introspection-adapting", null ],
        [ "Example: generating JSON", "index.html#tutorial-introspection-json", null ]
      ] ],
      [ "Generalities on containers", "index.html#tutorial-containers", [
        [ "Container creation", "index.html#tutorial-containers-creating", null ],
        [ "Container types", "index.html#tutorial-containers-types", [
          [ "Overloading on container types", "index.html#tutorial-containers-types-overloading", null ]
        ] ],
        [ "Container elements", "index.html#tutorial-containers-elements", null ]
      ] ],
      [ "Generalities on algorithms", "index.html#tutorial-algorithms", [
        [ "By-value semantics", "index.html#tutorial-algorithms-value", null ],
        [ "(Non-)Laziness", "index.html#tutorial-algorithms-laziness", null ],
        [ "What is generated?", "index.html#tutorial-algorithms-codegen", null ],
        [ "Side effects and purity", "index.html#tutorial-algorithms-effects", null ],
        [ "Cross-phase algorithms", "index.html#tutorial-algorithms-cross_phase", null ]
      ] ],
      [ "Performance considerations", "index.html#tutorial-performance", [
        [ "Compile-time performance", "index.html#tutorial-performance-compile", null ],
        [ "Runtime performance", "index.html#tutorial-performance-runtime", null ]
      ] ],
      [ "Integration with external libraries", "index.html#tutorial-ext", null ],
      [ "Hana's core", "index.html#tutorial-core", [
        [ "Tags", "index.html#tutorial-core-tags", null ],
        [ "Tag dispatching", "index.html#tutorial-core-tag_dispatching", null ],
        [ "Emulation of C++ concepts", "index.html#tutorial-core-concepts", null ]
      ] ],
      [ "Header organization", "index.html#tutorial-header_organization", null ],
      [ "Conclusion", "index.html#tutorial-conclusion", [
        [ "Fair warning: functional programming ahead", "index.html#tutorial-conclusion-warning", null ],
        [ "Related material", "index.html#tutorial-conclusion-related_material", null ],
        [ "Projects using Hana", "index.html#tutorial-conclusion-projects_using_hana", null ]
      ] ],
      [ "Using the reference", "index.html#tutorial-reference", [
        [ "Function signatures", "index.html#tutorial-reference-signatures", null ]
      ] ],
      [ "Acknowledgements", "index.html#tutorial-acknowledgements", null ],
      [ "Glossary", "index.html#tutorial-glossary", null ],
      [ "Rationales/FAQ", "index.html#tutorial-rationales", [
        [ "Why restrict usage of external dependencies?", "index.html#tutorial-rationales-dependencies", null ],
        [ "Why no iterators?", "index.html#tutorial-rationales-iterators", null ],
        [ "Why leave some container's representation implementation-defined?", "index.html#tutorial-rationales-container_representation", null ],
        [ "Why Hana?", "index.html#tutorial-rationales-why_Hana", null ],
        [ "Why define our own tuple?", "index.html#tutorial-rationales-tuple", null ],
        [ "How are names chosen?", "index.html#tutorial-rationales-naming", null ],
        [ "How is the parameter order decided?", "index.html#tutorial-rationales-parameters", null ],
        [ "Why tag dispatching?", "index.html#tutorial-rationales-tag_dispatching", null ],
        [ "Why not provide zip_longest?", "index.html#tutorial-rationales-zip_longest", null ],
        [ "Why aren't concepts constexpr functions?", "index.html#tutorial-rationales-concepts", null ]
      ] ],
      [ "Appendix I: Advanced constexpr", "index.html#tutorial-appendix-constexpr", [
        [ "Constexpr stripping", "index.html#tutorial-appendix-constexpr-stripping", null ],
        [ "Constexpr preservation", "index.html#tutorial-tutorial-appendix-constexpr-preservation", null ],
        [ "Side effects", "index.html#tutorial-appendix-constexpr-effects", null ]
      ] ]
    ] ],
    [ "Reference documentation", "modules.html", "modules" ],
    [ "Alphabetical index", "functions.html", null ],
    [ "Headers", "files.html", "files" ],
    [ "Todo List", "todo.html", null ],
    [ "Bug List", "bug.html", null ],
    [ "Deprecated List", "deprecated.html", null ],
    [ "deque", "structboost_1_1fusion_1_1deque.html", null ],
    [ "list", "structboost_1_1fusion_1_1list.html", null ],
    [ "tuple", "structboost_1_1fusion_1_1tuple.html", null ],
    [ "vector", "structboost_1_1fusion_1_1vector.html", null ],
    [ "which", "structboost_1_1hana_1_1constant__detail_1_1which.html", null ],
    [ "adl", "structboost_1_1hana_1_1detail_1_1operators_1_1adl.html", null ],
    [ "any_of", "structboost_1_1hana_1_1detail_1_1any__of.html", null ],
    [ "array", "structboost_1_1hana_1_1detail_1_1array.html", null ],
    [ "CanonicalConstant", "structboost_1_1hana_1_1detail_1_1_canonical_constant.html", null ],
    [ "create", "structboost_1_1hana_1_1detail_1_1create.html", null ],
    [ "decay", "structboost_1_1hana_1_1detail_1_1decay.html", null ],
    [ "first_unsatisfied_index", "structboost_1_1hana_1_1detail_1_1first__unsatisfied__index.html", null ],
    [ "has_duplicates", "structboost_1_1hana_1_1detail_1_1has__duplicates.html", null ],
    [ "nested_by", "structboost_1_1hana_1_1detail_1_1nested__by.html", null ],
    [ "nested_than", "structboost_1_1hana_1_1detail_1_1nested__than.html", null ],
    [ "nested_to", "structboost_1_1hana_1_1detail_1_1nested__to.html", null ],
    [ "std_common_type", "structboost_1_1hana_1_1detail_1_1std__common__type.html", null ],
    [ "type_at", "structboost_1_1hana_1_1detail_1_1type__at.html", null ],
    [ "wrong", "structboost_1_1hana_1_1detail_1_1wrong.html", null ],
    [ "has_common_embedding", "group__group-details.html#gae85b604ae6c7a386f0fc3631c561091b", null ],
    [ "has_nontrivial_common_embedding", "group__group-details.html#ga9acac3c4609cff5f0957572744c61ec4", null ],
    [ "types", "structboost_1_1hana_1_1experimental_1_1types.html", null ],
    [ "type_name", "group__group-experimental.html#gaf14876d1f1a3c42ce7a0243d7b263bec", null ],
    [ "print", "group__group-experimental.html#ga660c0769106006a86948b5b355fad050", null ],
    [ "operator\"\"_c", "namespaceboost_1_1hana_1_1literals.html#a85ac3c47d02722a334181aab540e732c", null ],
    [ "operator\"\"_s", "namespaceboost_1_1hana_1_1literals.html#a325859c7db2c3f8e6a4bfab5a81a6dcb", null ],
    [ "has_common", "structboost_1_1hana_1_1has__common.html", null ],
    [ "is_default", "structboost_1_1hana_1_1is__default.html", null ],
    [ "is_convertible", "structboost_1_1hana_1_1is__convertible.html", null ],
    [ "is_embedded", "structboost_1_1hana_1_1is__embedded.html", null ],
    [ "integral_constant_tag", "structboost_1_1hana_1_1integral__constant__tag.html", null ],
    [ "integral_constant", "structboost_1_1hana_1_1integral__constant.html", "structboost_1_1hana_1_1integral__constant" ],
    [ "basic_tuple", "structboost_1_1hana_1_1basic__tuple.html", "structboost_1_1hana_1_1basic__tuple" ],
    [ "basic_tuple_tag", "structboost_1_1hana_1_1basic__tuple__tag.html", null ],
    [ "IntegralConstant", "structboost_1_1hana_1_1_integral_constant.html", null ],
    [ "common", "structboost_1_1hana_1_1common.html", null ],
    [ "default_", "structboost_1_1hana_1_1default__.html", null ],
    [ "tag_of", "structboost_1_1hana_1_1tag__of.html", null ],
    [ "embedding", "structboost_1_1hana_1_1embedding.html", null ],
    [ "when", "structboost_1_1hana_1_1when.html", null ],
    [ "lazy", "structboost_1_1hana_1_1lazy.html", "structboost_1_1hana_1_1lazy" ],
    [ "lazy_tag", "structboost_1_1hana_1_1lazy__tag.html", null ],
    [ "map_tag", "structboost_1_1hana_1_1map__tag.html", null ],
    [ "map", "structboost_1_1hana_1_1map.html", "structboost_1_1hana_1_1map" ],
    [ "optional", "structboost_1_1hana_1_1optional.html", "structboost_1_1hana_1_1optional" ],
    [ "optional_tag", "structboost_1_1hana_1_1optional__tag.html", null ],
    [ "pair", "structboost_1_1hana_1_1pair.html", "structboost_1_1hana_1_1pair" ],
    [ "pair_tag", "structboost_1_1hana_1_1pair__tag.html", null ],
    [ "range", "structboost_1_1hana_1_1range.html", "structboost_1_1hana_1_1range" ],
    [ "range_tag", "structboost_1_1hana_1_1range__tag.html", null ],
    [ "set", "structboost_1_1hana_1_1set.html", "structboost_1_1hana_1_1set" ],
    [ "set_tag", "structboost_1_1hana_1_1set__tag.html", null ],
    [ "string", "structboost_1_1hana_1_1string.html", "structboost_1_1hana_1_1string" ],
    [ "string_tag", "structboost_1_1hana_1_1string__tag.html", null ],
    [ "tuple", "structboost_1_1hana_1_1tuple.html", "structboost_1_1hana_1_1tuple" ],
    [ "tuple_tag", "structboost_1_1hana_1_1tuple__tag.html", null ],
    [ "basic_type", "structboost_1_1hana_1_1basic__type.html", null ],
    [ "type", "structboost_1_1hana_1_1type.html", "structboost_1_1hana_1_1type" ],
    [ "type_tag", "structboost_1_1hana_1_1type__tag.html", null ],
    [ "common_t", "group__group-core.html#ga4da46c97755c0f430b063711b66ca05b", null ],
    [ "tag_of_t", "group__group-core.html#ga686d1236161b5690ab302500077988e1", null ],
    [ "when_valid", "group__group-core.html#ga0f5d717bbf6646619bb6219b104384dc", null ],
    [ "BOOST_HANA_ADAPT_ADT", "group__group-_struct.html#ga141761435a7826b3cbe646b4f59eaf0a", null ],
    [ "BOOST_HANA_ADAPT_STRUCT", "group__group-_struct.html#gaba3b4d2cf342bfca773e90fc20bfae91", null ],
    [ "A", "group__group-_applicative.html#ga4b7188568b24c715ec8e43595de6844d", null ],
    [ "BOOST_HANA_DEFINE_STRUCT", "group__group-_struct.html#gab9efb238a82207d91643994c5295cf8c", null ],
    [ "always", "group__group-functional.html#ga835970cb25a0c8dc200f1e5f8943538b", null ],
    [ "apply", "group__group-functional.html#ga30027c383676084be151ef3c6cf2829f", null ],
    [ "arg", "group__group-functional.html#ga6acc765a35c4dc85f0deab4785831a3d", null ],
    [ "capture", "group__group-functional.html#ga41ada6b336e9d5bcb101ff0c737acbd0", null ],
    [ "compose", "group__group-functional.html#ga3b16146e53efcdf9ecbb9a7b21f8cd0b", null ],
    [ "curry", "group__group-functional.html#ga49ea872ade5ac8f6c10052c495302e89", null ],
    [ "mathtt", "group__group-functional.html#ga8c6f17b58ce527c7650eb878f01f2cd2", null ],
    [ "fix", "group__group-functional.html#ga1393f40da2e8da6e0c12fce953e56a6c", null ],
    [ "flip", "group__group-functional.html#ga004f884cdbb85c2efe3383c1db450094", null ],
    [ "id", "group__group-functional.html#gaef38cf34324c8edbd3597ae71811d00d", null ],
    [ "infix", "group__group-functional.html#ga7bdafba6dc801f1d2d83731ad9714557", null ],
    [ "lockstep", "group__group-functional.html#gafca60c09e1f7a32a2b52baaf6515c279", null ],
    [ "on", "group__group-functional.html#ga35c4fc3c5677b9f558150b90e74d3ab1", null ],
    [ "overload", "group__group-functional.html#ga83e71bae315e299f9f5f9de77b012139", null ],
    [ "overload_linearly", "group__group-functional.html#gaa46de6f618d9f14edb1589b36b6e75ec", null ],
    [ "partial", "group__group-functional.html#ga778b2daa27882e71d28b6f2b38982ddf", null ],
    [ "_", "group__group-functional.html#gaefe9fd152cba94be71c2b5b9de689d23", null ],
    [ "reverse_partial", "group__group-functional.html#ga6e648f0d3fc0209ec024e9d759a5e8f8", null ],
    [ "accessors", "group__group-_struct.html#ga983a55dbd93d766fd37689ea32e4ddfb", null ],
    [ "all", "group__group-_searchable.html#ga81ae9764dd7818ad36270c6419fb1082", null ],
    [ "all_of", "group__group-_searchable.html#ga3a168950082f38afd9edf256f336c8ba", null ],
    [ "and_", "group__group-_logical.html#ga14066f5672867c123524e0e0978069eb", null ],
    [ "any", "group__group-_searchable.html#gab7d632b9319b10b1eb7e98f9e1cf8a28", null ],
    [ "any_of", "group__group-_searchable.html#ga5f7ff0125c448983e1b96c3ffb84f646", null ],
    [ "append", "group__group-_monad_plus.html#ga08624924fe05f0cfbfbd6e439db01873", null ],
    [ "at", "group__group-_iterable.html#ga8a484304380eae38f3d9663d98860129", null ],
    [ "at_c", "group__group-_iterable.html#ga4cb99cfbef936cb267e76f66f40f529c", null ],
    [ "at_key", "group__group-_searchable.html#ga3c1826aee6c6eb577810bb99c5c3e53d", null ],
    [ "back", "group__group-_iterable.html#gab3f4d0035345a453284e46303862d463", null ],
    [ "comparing", "group__group-_comparable.html#ga9c2ffe2e51780e57a38d9e7e31b87cdc", null ],
    [ "concat", "group__group-_monad_plus.html#ga1946e96c3b4c178c7ae8703724c29c37", null ],
    [ "contains", "group__group-_searchable.html#ga38e7748956cbc9f3d9bb035ac8577906", null ],
    [ "in", "group__group-_searchable.html#ga0d9456ceda38b6ca664998e79d7c45b7", null ],
    [ "is_a", "group__group-core.html#ga38cf78e1e3e262f7f1c71ddd9ca70cd9", null ],
    [ "is_an", "group__group-core.html#ga7fdbde52f5fe384a816c6f39ff272df9", null ],
    [ "make", "group__group-core.html#ga1d92480f0af1029878e773dafa3e2f60", null ],
    [ "to", "group__group-core.html#gadc70755c1d059139297814fb3bfeb91e", null ],
    [ "count", "group__group-_foldable.html#ga3159cfa41be18a396926741b0a3fdefd", null ],
    [ "count_if", "group__group-_foldable.html#ga39d71be65d5b98e7d035a3e5c607e1b4", null ],
    [ "cycle", "group__group-_monad_plus.html#gaaf46c168f721da9effcc7336a997f5d6", null ],
    [ "div", "group__group-_euclidean_ring.html#ga4225a7988ce98903228913dde53762df", null ],
    [ "drop_back", "group__group-_sequence.html#gac10231310abc86b056585ea0d0e96ef7", null ],
    [ "drop_front", "group__group-_iterable.html#gad23ce0a4906e2bb0a52f38837b134757", null ],
    [ "drop_front_exactly", "group__group-_iterable.html#ga4dbc6a82f03ca35b7ac418ca30889cc4", null ],
    [ "drop_while", "group__group-_iterable.html#ga9f1d02c74a6bdc1db260e0d6a8f1ee56", null ],
    [ "empty", "group__group-_monad_plus.html#gaa6be1e83ad72b9d69b43b4bada0f3a75", null ],
    [ "equal", "group__group-_comparable.html#gacaf1ebea6b3ab96ac9dcb82f0e64e547", null ],
    [ "eval_if", "group__group-_logical.html#gab64636f84de983575aac0208f5fa840c", null ],
    [ "filter", "group__group-_monad_plus.html#ga65cc6d9f522fb9e8e3b28d80ee5c822a", null ],
    [ "find", "group__group-_searchable.html#ga6b6cdd69942b0fe3bf5254247f9c861e", null ],
    [ "find_if", "group__group-_searchable.html#ga7f99b80672aa80a7eb8b223955ce546f", null ],
    [ "first", "group__group-_product.html#ga34bbf4281de06dc3540441e8b2bd24f4", null ],
    [ "fold", "group__group-_foldable.html#gaa0fde17f3b947a0678a1c0c01232f2cc", null ],
    [ "for_each", "group__group-_foldable.html#ga2af382f7e644ce3707710bbad313e9c2", null ],
    [ "front", "group__group-_iterable.html#ga8a67ea10e8082dbe6705e573fa978444", null ],
    [ "fuse", "group__group-_foldable.html#ga19fcf61d8d1179903952c0f564c538aa", null ],
    [ "greater", "group__group-_orderable.html#gaf9a073eafebbe514fb19dff82318f198", null ],
    [ "greater_equal", "group__group-_orderable.html#ga6023631e7d0a01e16dc3fa4221fbd703", null ],
    [ "if_", "group__group-_logical.html#gafd655d2222367131e7a63616e93dd080", null ],
    [ "index_if", "group__group-_iterable.html#ga5332fd1dd82edf08379958ba21d57a87", null ],
    [ "insert", "group__group-_sequence.html#gae22a1a184b1b2dd550fa4fa619bed2e9", null ],
    [ "insert_range", "group__group-_sequence.html#ga3410ba833cf1ff1d929fcfda4df2eae1", null ],
    [ "intersperse", "group__group-_sequence.html#gaa18061cd0f63cfaae89abf43ff92b79e", null ],
    [ "is_disjoint", "group__group-_searchable.html#ga3b8269d4f5cdd6dd549fae32280795a0", null ],
    [ "is_empty", "group__group-_iterable.html#ga2a05f564f8a7e4afa04fcbc07ad8f394", null ],
    [ "is_subset", "group__group-_searchable.html#gadccfc79f1acdd8043d2baa16df16ec9f", null ],
    [ "keys", "group__group-_struct.html#gaf8c7199742581e6e66c8397def68e2d3", null ],
    [ "length", "group__group-_foldable.html#gaf0f8f717245620dc28cd7d7fa44d7475", null ],
    [ "less", "group__group-_orderable.html#gad510011602bdb14686f1c4ec145301c9", null ],
    [ "less_equal", "group__group-_orderable.html#ga9917dd82beb67151bf5657245d37b851", null ],
    [ "lift", "group__group-_applicative.html#ga712038d7abbc7159f8792788f7cd0c73", null ],
    [ "max", "group__group-_orderable.html#ga999eee8ca8750f9b1afa0d7a1db28030", null ],
    [ "members", "group__group-_struct.html#gad301dd8e9fb4639d7874619c97d6d427", null ],
    [ "min", "group__group-_orderable.html#ga2d54f189ea6f57fb2c0d772169440c5c", null ],
    [ "minus", "group__group-_group.html#ga2020c526324f361a2b990fe8d1b07c20", null ],
    [ "mod", "group__group-_euclidean_ring.html#ga9b47b223d5b02db933b3c93b5bd1a062", null ],
    [ "mult", "group__group-_ring.html#ga052d31c269a6a438cc8004c9ad1efdfa", null ],
    [ "negate", "group__group-_group.html#ga02e81002f40ba52eac4cf1974c7e0cdb", null ],
    [ "none", "group__group-_searchable.html#ga614ff1e575806f59246b17006e19d479", null ],
    [ "none_of", "group__group-_searchable.html#ga43954c791b5b1351fb009e2a643d00f5", null ],
    [ "not_", "group__group-_logical.html#ga4a7c9d7037601d5e553fd20777958980", null ],
    [ "not_equal", "group__group-_comparable.html#gae33be2e0d5e04f19082f4b7740dfc9cd", null ],
    [ "one", "group__group-_ring.html#gadea531feb3b0a1c5c3d777f7ab45e932", null ],
    [ "or_", "group__group-_logical.html#ga68c00efbeb69339bfa157a78ebdd3f87", null ],
    [ "ordering", "group__group-_orderable.html#gaf7e94ba859710cd6ba6152e5dc18977d", null ],
    [ "permutations", "group__group-_sequence.html#gac1e182ac088f1990edd739424d30ea07", null ],
    [ "plus", "group__group-_monoid.html#gaeb5d4a1e967e319712f9e4791948896c", null ],
    [ "power", "group__group-_ring.html#ga0ee3cff9ec646bcc7217f00ee6099b72", null ],
    [ "prefix", "group__group-_monad_plus.html#ga3022fdfe454dc9bc1f79b5dfeba13b5e", null ],
    [ "prepend", "group__group-_monad_plus.html#ga69afbfd4e91125e3e52fcb409135ca7c", null ],
    [ "product", "group__group-_foldable.html#ga17fe9c1982c882807f3358b4138c5744", null ],
    [ "mathrm", "group__group-_monad_plus.html#ga5ee54dc1195f9e5cf48bfd51ba231ae5", null ],
    [ "remove_at", "group__group-_sequence.html#ga80724ec8ecf319a1e695988a69e22f87", null ],
    [ "remove_at_c", "group__group-_sequence.html#gae70b0815645c7d81bb636a1eed1a65c6", null ],
    [ "remove_range", "group__group-_sequence.html#ga6f6d5c1f335780c91d29626fde615c78", null ],
    [ "remove_range_c", "group__group-_sequence.html#ga4696efcdee7d95ab4a391bb896a840b5", null ],
    [ "repeat", "namespaceboost_1_1hana.html#a405f3dd84fc6f5003e64f8da104a1b54", null ],
    [ "replicate", "group__group-_monad_plus.html#gad5f48c79d11923d6c1d70b18b7dd3f19", null ],
    [ "reverse", "group__group-_sequence.html#ga28037560e8f224c53cf6ac168d03a067", null ],
    [ "scan_left", "group__group-_sequence.html#gaec484fb349500149d90717f6e68f7bcd", null ],
    [ "scan_right", "group__group-_sequence.html#ga54d141f901866dfab29b052857123bab", null ],
    [ "second", "group__group-_product.html#ga7bb979d59ffc3ab862cb7d9dc7730077", null ],
    [ "size", "group__group-_foldable.html#ga8ec3ac9a6f5014db943f61ebc9e1e36e", null ],
    [ "slice", "group__group-_sequence.html#ga245d8abaf6ba67e64020be51c8366081", null ],
    [ "slice_c", "group__group-_sequence.html#gae1f6a2a9cb70564d43c6b3c663b25dd7", null ],
    [ "suffix", "group__group-_monad_plus.html#ga61dab15f6ecf379121d4096fe0c8ab13", null ],
    [ "sum", "group__group-_foldable.html#ga650def4b2e98f4273d8b9b7aa5a2fc28", null ],
    [ "take_back", "group__group-_sequence.html#ga8d302de01b94b4b17f3bd81e09f42920", null ],
    [ "take_back_c", "group__group-_sequence.html#gaa4d4818952083e3b27c83b0ed645e322", null ],
    [ "take_front", "group__group-_sequence.html#ga5112e6070d29b4f7fde3f44825da3316", null ],
    [ "take_front_c", "group__group-_sequence.html#ga3779f62fea92af00113a9290f1c680eb", null ],
    [ "take_while", "group__group-_sequence.html#ga2d4db4ec5ec5bc16fe74f57de12697fd", null ],
    [ "tap", "group__group-_monad.html#ga5e0735de01a24f681c55aedfeb6d13bf", null ],
    [ "then", "group__group-_monad.html#gaaddd3789de43cf989babb10cdc0b447a", null ],
    [ "template_", "group__group-_metafunction.html#ga246419f6c3263b648412f346106e6543", null ],
    [ "metafunction", "group__group-_metafunction.html#gaaa4f85cb8cbce21f5c04ef40ca35cc6a", null ],
    [ "metafunction_class", "group__group-_metafunction.html#gacec153d7f86aa7cf1efd813b3fd212b4", null ],
    [ "integral", "group__group-_metafunction.html#gaf7045fe6a627f88f5f646dad22d37aae", null ],
    [ "trait", "group__group-_metafunction.html#ga6d4093318f46472e62f9539a4dc998a9", null ],
    [ "unpack", "group__group-_foldable.html#ga7b0c23944364ce61136e10b978ae2170", null ],
    [ "value", "group__group-_constant.html#ga1687520692a6b0c49e3a69de2980f388", null ],
    [ "value_of", "group__group-_constant.html#gab46a092deeb205f2c92c335d4312a991", null ],
    [ "while_", "group__group-_logical.html#ga08a767b86c330cac67daa891406d2730", null ],
    [ "zero", "group__group-_monoid.html#gad459ac17b6bab8ead1cae7de0032f3c6", null ],
    [ "zip", "group__group-_sequence.html#gaa5a378d4e71a91e0d6cd3959d9818e8a", null ],
    [ "zip_shortest", "group__group-_sequence.html#gade78593b3ff51fc5479e1da97142fef5", null ],
    [ "zip_shortest_with", "group__group-_sequence.html#gae7a51104a77db79a0407d7d67b034667", null ],
    [ "zip_with", "group__group-_sequence.html#ga6a4bf8549ce69b5b5b7377aec225a0e3", null ],
    [ "integral_c", "structboost_1_1mpl_1_1integral__c.html", null ],
    [ "list", "structboost_1_1mpl_1_1list.html", null ],
    [ "vector", "structboost_1_1mpl_1_1vector.html", null ],
    [ "tuple", "structboost_1_1tuple.html", null ],
    [ "array", "structstd_1_1array.html", null ],
    [ "integer_sequence", "structstd_1_1integer__sequence.html", null ],
    [ "integral_constant", "structstd_1_1integral__constant.html", null ],
    [ "pair", "structstd_1_1pair.html", null ],
    [ "ratio", "classstd_1_1ratio.html", null ],
    [ "tuple", "structstd_1_1tuple.html", null ],
    [ "deque", "structboost_1_1fusion_1_1deque.html", null ],
    [ "list", "structboost_1_1fusion_1_1list.html", null ],
    [ "tuple", "structboost_1_1fusion_1_1tuple.html", null ],
    [ "vector", "structboost_1_1fusion_1_1vector.html", null ],
    [ "which", "structboost_1_1hana_1_1constant__detail_1_1which.html", null ],
    [ "adl", "structboost_1_1hana_1_1detail_1_1operators_1_1adl.html", null ],
    [ "any_of", "structboost_1_1hana_1_1detail_1_1any__of.html", null ],
    [ "array", "structboost_1_1hana_1_1detail_1_1array.html", null ],
    [ "CanonicalConstant", "structboost_1_1hana_1_1detail_1_1_canonical_constant.html", null ],
    [ "create", "structboost_1_1hana_1_1detail_1_1create.html", null ],
    [ "decay", "structboost_1_1hana_1_1detail_1_1decay.html", null ],
    [ "first_unsatisfied_index", "structboost_1_1hana_1_1detail_1_1first__unsatisfied__index.html", null ],
    [ "has_duplicates", "structboost_1_1hana_1_1detail_1_1has__duplicates.html", null ],
    [ "nested_by", "structboost_1_1hana_1_1detail_1_1nested__by.html", null ],
    [ "nested_than", "structboost_1_1hana_1_1detail_1_1nested__than.html", null ],
    [ "nested_to", "structboost_1_1hana_1_1detail_1_1nested__to.html", null ],
    [ "std_common_type", "structboost_1_1hana_1_1detail_1_1std__common__type.html", null ],
    [ "type_at", "structboost_1_1hana_1_1detail_1_1type__at.html", null ],
    [ "wrong", "structboost_1_1hana_1_1detail_1_1wrong.html", null ],
    [ "types", "structboost_1_1hana_1_1experimental_1_1types.html", null ],
    [ "has_common", "structboost_1_1hana_1_1has__common.html", null ],
    [ "is_default", "structboost_1_1hana_1_1is__default.html", null ],
    [ "is_convertible", "structboost_1_1hana_1_1is__convertible.html", null ],
    [ "is_embedded", "structboost_1_1hana_1_1is__embedded.html", null ],
    [ "integral_constant_tag", "structboost_1_1hana_1_1integral__constant__tag.html", null ],
    [ "integral_constant", "structboost_1_1hana_1_1integral__constant.html", "structboost_1_1hana_1_1integral__constant" ],
    [ "basic_tuple", "structboost_1_1hana_1_1basic__tuple.html", "structboost_1_1hana_1_1basic__tuple" ],
    [ "basic_tuple_tag", "structboost_1_1hana_1_1basic__tuple__tag.html", null ],
    [ "IntegralConstant", "structboost_1_1hana_1_1_integral_constant.html", null ],
    [ "common", "structboost_1_1hana_1_1common.html", null ],
    [ "default_", "structboost_1_1hana_1_1default__.html", null ],
    [ "tag_of", "structboost_1_1hana_1_1tag__of.html", null ],
    [ "embedding", "structboost_1_1hana_1_1embedding.html", null ],
    [ "when", "structboost_1_1hana_1_1when.html", null ],
    [ "lazy", "structboost_1_1hana_1_1lazy.html", "structboost_1_1hana_1_1lazy" ],
    [ "lazy_tag", "structboost_1_1hana_1_1lazy__tag.html", null ],
    [ "map_tag", "structboost_1_1hana_1_1map__tag.html", null ],
    [ "map", "structboost_1_1hana_1_1map.html", "structboost_1_1hana_1_1map" ],
    [ "optional", "structboost_1_1hana_1_1optional.html", "structboost_1_1hana_1_1optional" ],
    [ "optional_tag", "structboost_1_1hana_1_1optional__tag.html", null ],
    [ "pair", "structboost_1_1hana_1_1pair.html", "structboost_1_1hana_1_1pair" ],
    [ "pair_tag", "structboost_1_1hana_1_1pair__tag.html", null ],
    [ "range", "structboost_1_1hana_1_1range.html", "structboost_1_1hana_1_1range" ],
    [ "range_tag", "structboost_1_1hana_1_1range__tag.html", null ],
    [ "set", "structboost_1_1hana_1_1set.html", "structboost_1_1hana_1_1set" ],
    [ "set_tag", "structboost_1_1hana_1_1set__tag.html", null ],
    [ "string", "structboost_1_1hana_1_1string.html", "structboost_1_1hana_1_1string" ],
    [ "string_tag", "structboost_1_1hana_1_1string__tag.html", null ],
    [ "tuple", "structboost_1_1hana_1_1tuple.html", "structboost_1_1hana_1_1tuple" ],
    [ "tuple_tag", "structboost_1_1hana_1_1tuple__tag.html", null ],
    [ "basic_type", "structboost_1_1hana_1_1basic__type.html", null ],
    [ "type", "structboost_1_1hana_1_1type.html", "structboost_1_1hana_1_1type" ],
    [ "type_tag", "structboost_1_1hana_1_1type__tag.html", null ],
    [ "integral_c", "structboost_1_1mpl_1_1integral__c.html", null ],
    [ "list", "structboost_1_1mpl_1_1list.html", null ],
    [ "vector", "structboost_1_1mpl_1_1vector.html", null ],
    [ "tuple", "structboost_1_1tuple.html", null ],
    [ "array", "structstd_1_1array.html", null ],
    [ "integer_sequence", "structstd_1_1integer__sequence.html", null ],
    [ "integral_constant", "structstd_1_1integral__constant.html", null ],
    [ "pair", "structstd_1_1pair.html", null ],
    [ "ratio", "classstd_1_1ratio.html", null ],
    [ "tuple", "structstd_1_1tuple.html", null ]
  ] ]
];

var NAVTREEINDEX =
[
"accessors_8hpp.html",
"fwd_2cycle_8hpp.html",
"group__group-_iterable.html#ga5332fd1dd82edf08379958ba21d57a87",
"index.html#tutorial-integral-distance",
"structboost_1_1hana_1_1pair.html#a962bff38110b5c39b1267fc88851198d"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';