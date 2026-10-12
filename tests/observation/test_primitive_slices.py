#!/usr/bin/env python3
"""Frozen primitive-slice qualification: pure prepare/mocks, no-launch IR, native positives.

R2 oracles were independently reviewed before compiler evaluation. Expected
results must never be changed to match implementation output. The 174 retained
negative sources and 22 count controls are excluded from native dispatch.
Native malloc-null is NOT_RUN / outside initial native qualification.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
if not __debug__:
    raise RuntimeError("qualification requires Python assertions enabled")
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/observation/fixtures/primitive_slices"
FAMILY = "observed-primitive-slices"
PROFILE = "native-sequential-direct-source-positional-slice-v4"
CONTRACT_SHA256 = "27ff2b8d2ca38131b4b86224211758f0c3b23dd6d16f4f7bfa5bbaa30fe5c62c"
REVIEWED_ORACLE_SHA256 = "47cdb29db83fac25d3153cc4aa9d32ad0df70e074503d4794b97eba391e70a67"
REVIEWED_SOURCE_MANIFEST_SHA256 = "8d069d02a06d1023f524c50cc924a36e327f2991437d660fbc1d1bac70eace6d"
NATIVE_OOM = "NOT_RUN / outside initial native qualification"
_spec = importlib.util.spec_from_file_location("slice_source_support", ROOT / "tests/observation/test_source_imports.py")
support = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(support)
sha = support.sha

# Literal independently reviewed source and oracle inventories. No compiler output
# or filesystem glob establishes case enrollment or expected semantic results.
NATIVE_NAMES = ('coverage',
 'count_order',
 'same_line',
 'imports',
 'primitive_i8',
 'primitive_u8',
 'primitive_i16',
 'primitive_u16',
 'primitive_i32',
 'primitive_u32',
 'primitive_i64',
 'primitive_u64',
 'primitive_bool',
 'primitive_f32',
 'primitive_f64',
 'count_i8',
 'count_u8',
 'count_i16',
 'count_u16',
 'count_i32',
 'count_u32',
 'count_i64',
 'count_u64',
 'count_bool',
 'count_unsigned_top_bit',
 'retained_i32',
 'retained_empty',
 'omitted_i16',
 'zero_omitted',
 'zero_explicit',
 'mixed_both',
 'mixed_explicit_only',
 'mixed_slice_only',
 'schedule_direct',
 'schedule_defer_fallthrough',
 'schedule_defer_return',
 'schedule_defer_nested',
 'schedule_defer_continue',
 'schedule_defer_break',
 'schedule_defer_nested_exits',
 'schedule_direct_bypass_return',
 'schedule_direct_bypass_break',
 'schedule_direct_bypass_continue',
 'schedule_shadow_defer',
 'schedule_shadow_siblings',
 'nested_scalar',
 'no_assertions')

BELOW_NAMES = ('retained_i32',
 'retained_empty',
 'omitted_i16',
 'mixed_both',
 'mixed_explicit_only',
 'mixed_slice_only',
 'schedule_direct_bypass_return',
 'schedule_direct_bypass_break',
 'schedule_direct_bypass_continue',
 'imports')

FIXTURE_HASHES = {'count_order.sts': 'a366cb8ecb4fb60f73c0b77e98b327351207c2f3249ce9c6e04d3e9cd0fbea2e',
 'coverage.sts': 'b2fd1ba74357786039b30da03c833c488bb8ad2c219fa2517edcfcec7a8f0105',
 'imports/root.sts': '27a74e34315887c0ccc30d5c0d9dccda0ea1cd560353f917bb303e90f9b24c52',
 'imports/util/slice.sts': 'e3d0a0b59b9db1a141ba48be9b4388892229d39a6c4fdb4adc00183fdd146f4c',
 'same_line.sts': '0b053cc9ed36792cabcaabae51e975b73edc3d8b61e4d9997fcf404344e1c6fb'}

SOURCE_HASHES = {'fixtures/primitive_slices/count_order.sts': 'a366cb8ecb4fb60f73c0b77e98b327351207c2f3249ce9c6e04d3e9cd0fbea2e',
 'fixtures/primitive_slices/coverage.sts': 'b2fd1ba74357786039b30da03c833c488bb8ad2c219fa2517edcfcec7a8f0105',
 'fixtures/primitive_slices/imports/root.sts': '27a74e34315887c0ccc30d5c0d9dccda0ea1cd560353f917bb303e90f9b24c52',
 'fixtures/primitive_slices/imports/util/slice.sts': 'e3d0a0b59b9db1a141ba48be9b4388892229d39a6c4fdb4adc00183fdd146f4c',
 'fixtures/primitive_slices/same_line.sts': '0b053cc9ed36792cabcaabae51e975b73edc3d8b61e4d9997fcf404344e1c6fb',
 'sources/primitive-slices/count-failure/cap_i16_neg.sts': 'e87cc2e7eebeac6b4120e710d3ad3972219811efbf0709dc9f837fdf807fda04',
 'sources/primitive-slices/count-failure/cap_i32_neg.sts': 'f8480f86fa2b72cfabdf07c10223c9f941a93081d3d482e824edba1cec9b5b40',
 'sources/primitive-slices/count-failure/cap_i64_neg.sts': 'ca8f133faefc6aaab5adcb34c672b6ed329a482dd659f895a987cd7457679460',
 'sources/primitive-slices/count-failure/cap_i64_over.sts': '6881447bef071182349a84acfc89adb139c0902f2cf312ed38a6956b5881d416',
 'sources/primitive-slices/count-failure/cap_i8_neg.sts': 'fc1999d052d7047512402021012ca6ce8491f89a1cd66c36dcd9f0eb310ae729',
 'sources/primitive-slices/count-failure/cap_u32_max.sts': '8edd1e566fd368d245f0bac0f24f75adae3db391f51c2f6e5c4654360413fa3f',
 'sources/primitive-slices/count-failure/cap_u32_over.sts': '70d882c2a3fe724de00a7c005b8c9f12a6f3731a20c7ef1b6521be0ce48c049f',
 'sources/primitive-slices/count-failure/cap_u64_max.sts': 'c75ad69754d545fa00603204fcde933f9ae054d47c12e0a40ae02dd1042a1a01',
 'sources/primitive-slices/count-failure/cap_u64_wrap.sts': '88e49019438ae0b8bb0b47dc77a687d84fcf48ebf3ccaddb9bcfc971751952e5',
 'sources/primitive-slices/count-failure/cap_u64_wrap1.sts': '999cf46fd43834b19dd08fdc287694ae4f69af8a022f4c4d354f0fd49f199285',
 'sources/primitive-slices/count-failure/invalid_order.sts': 'b2b3307e858b2b51bc6c9b2135b1500b7a41b1a613c2bcc31db7c12e1f755f1b',
 'sources/primitive-slices/count-failure/len_gt_cap.sts': 'f8da152afdfa4a57f72ff3257a74b5d6e0d5525bbcd78fa73e0bb966bef10827',
 'sources/primitive-slices/count-failure/len_i16_neg.sts': '9cb350ce8f82c4209b650d8b1a4851f4ff03457eab5701cc5296fa18f371b610',
 'sources/primitive-slices/count-failure/len_i32_neg.sts': '308646e657b8ecf51b38d421b6c5c6859ebd81e21bafbae9290fbaddb92f2eba',
 'sources/primitive-slices/count-failure/len_i64_neg.sts': '9c7c9fa088615dd756979f5a359c6a5b4fc17d57cc701a5e97f04a5bc99ae112',
 'sources/primitive-slices/count-failure/len_i64_over.sts': 'e6acf00016db927d97858a33a9bdd0a44c7d4222932e028babe647d6a5fe216b',
 'sources/primitive-slices/count-failure/len_i8_neg.sts': '2d030c1fa5502572fdde84c9c7a6bc155777cb8a491d101fcc84cbe926edc69a',
 'sources/primitive-slices/count-failure/len_u32_max.sts': '82fa0f7c821a05352b989b6dad37bb73019a1e1ce6c62bf4fa58b90d1fa12e74',
 'sources/primitive-slices/count-failure/len_u32_over.sts': 'e38a6a8f677fedf97bcfd67cde68beebf6924acf57e72375bd23f2da55f35967',
 'sources/primitive-slices/count-failure/len_u64_max.sts': '4169e8f5b77ee9613595c7c607d1a757a1935eb57e31deb29e464e6e4cac2d7c',
 'sources/primitive-slices/count-failure/len_u64_wrap.sts': 'e7271792244580c68149fb33b5d0fa1adf6650f1f290cf684fb246dec876ca1b',
 'sources/primitive-slices/count-failure/len_u64_wrap1.sts': '3c5a54f35e8a8df69cce59c132dbd69abf30c8ba7fda455c8c85fdd570fedcb5',
 'sources/primitive-slices/native/count_bool.sts': '489d827471914026d0d524c80a994db0881f36e71a64cf973f68a9285b8aadde',
 'sources/primitive-slices/native/count_i16.sts': 'f05facca526fc60c569a43fb5e729ffe793f45815cf413f86d6783826f1d6c72',
 'sources/primitive-slices/native/count_i32.sts': '068fdaabf9d1c994dea6a27ccca4472ff2d6cac7a6f0ed358bae6fa8a5a1346a',
 'sources/primitive-slices/native/count_i64.sts': 'b315fe46b681f9d42358a96a263d8cbc7f5833e49a8fd755ad21f017f718f836',
 'sources/primitive-slices/native/count_i8.sts': 'cb6472a6156d4e7f437e85879a6530ea14353cc499cf7c01760d7b92032baa99',
 'sources/primitive-slices/native/count_u16.sts': 'e22f21a92e2490d2bc3370f1702ee936016c71e70e99fb8403b41831243c87b6',
 'sources/primitive-slices/native/count_u32.sts': 'a05b21868089edfd025ac4598dcca28e506f86199e433f60a666c26443dda53f',
 'sources/primitive-slices/native/count_u64.sts': '1b6e4ca8214f27fa00587bd509334ff1fbe3308c5f5ecd908f7bd9039335e2f3',
 'sources/primitive-slices/native/count_u8.sts': '8f08cb55b7efcf49bad91c4caf34843760178b0b6df0b0dd6f2edce8c2dacab0',
 'sources/primitive-slices/native/count_unsigned_top_bit.sts': 'd0a867cdd744fa22d4cb644631211d814eed6c285b45c15cda57a79b52c9b48b',
 'sources/primitive-slices/native/mixed_both.sts': '0ca31d0be5bec29ce3f987fadf3c057e33a26350f167a069e755e252cf3faa68',
 'sources/primitive-slices/native/mixed_explicit_only.sts': 'e86a4aeeaf50b2cb7a6c9863f0213e1b718fe4354ac6a4d046a7ae6786bd5622',
 'sources/primitive-slices/native/mixed_slice_only.sts': 'a05dd4d9a2c1de64d36989603a268a32c26c9c1798858331e53c47c79b9d0806',
 'sources/primitive-slices/native/nested_scalar.sts': '1082856bf8eef1366033409e24d4d242d54c7b1664a3ebe5868199d9efc72c8e',
 'sources/primitive-slices/native/no_assertions.sts': 'f2b9dd63799c68021f227c94da46c377c4f549ceddbe3f630764d65c24529c26',
 'sources/primitive-slices/native/omitted_i16.sts': '47f0ba3cf1d8e114f268ccb689bf80094a52e591d65790b4ac9553cdd2f1e800',
 'sources/primitive-slices/native/primitive_bool.sts': '94eea887ce5850557e49a020893a4e28869e50d59810b0f4ff544f1b238ae6bf',
 'sources/primitive-slices/native/primitive_f32.sts': '9172f68a09b83c6e40ffd84077958bb39d730c1c8c98beaab273ac173a77b5e0',
 'sources/primitive-slices/native/primitive_f64.sts': 'f35de30d93f5b44a834808bda99f245a8507a323a85b887b154c5ed6a4de9bf4',
 'sources/primitive-slices/native/primitive_i16.sts': '0b31cf433cf40f5ccdd6887a2db162ad6654a126c8fce86c056229a64e850b30',
 'sources/primitive-slices/native/primitive_i32.sts': '79520c94d688a125ae9676dbe2bb5acb5ea8bd1bf3da5fe2644ad339ed0267a8',
 'sources/primitive-slices/native/primitive_i64.sts': '15abeae77a0b36f720837f5aa0405c142210ba37bef169a038e7fbc94bd69cad',
 'sources/primitive-slices/native/primitive_i8.sts': 'c4f0cd1ae0a0820dc06480bcee24eb3d8d8e3d8f8222b0a8418ad3768d7df4ef',
 'sources/primitive-slices/native/primitive_u16.sts': '3cdb9a7eb024ee44fd28a4cdc680043b84798ebf09b4e6b1da75c84d7eff4c2f',
 'sources/primitive-slices/native/primitive_u32.sts': 'd30899d200ae643bdbb51cdbb1e2befb7c99d33d98e8838de582763e65a94942',
 'sources/primitive-slices/native/primitive_u64.sts': '104296b56eaf09fc0e060031721a691ff2af025d32933da1e27f4351242dc9b7',
 'sources/primitive-slices/native/primitive_u8.sts': '2ac3a79e1f975f53b5ba0098457015503534879ff451c1dccf30766dbf722fed',
 'sources/primitive-slices/native/retained_empty.sts': '486354fa7128aef28e86c8606a906fd58b2a39d52a2bc50c3692f442ef7c6b25',
 'sources/primitive-slices/native/retained_i32.sts': '64bbcd74d351b9e8e1f13c9ab8984df2d4a8fcf0e329126d87dfe7ff4cc51a51',
 'sources/primitive-slices/native/schedule_defer_break.sts': '531bf3ce44b35e6453e431486ac3057266ed5f19d65a1f54717e86aa63a878b8',
 'sources/primitive-slices/native/schedule_defer_continue.sts': '880ef333b52193cac79551a03021baabeb8f799b439e176acfe88ba4cf0afac3',
 'sources/primitive-slices/native/schedule_defer_fallthrough.sts': '6a79991613c4297f4ee4bc3f102de188d1e20dd6a89f6931d89fd4427ce70fbe',
 'sources/primitive-slices/native/schedule_defer_nested.sts': 'e45a8b11daa204f0082e6e7a57d6283d4226afbb0eee71d756eea40dbe1ec835',
 'sources/primitive-slices/native/schedule_defer_nested_exits.sts': '31817278d0cf0c0a9d1453893cb4a455155ef1c29f54f32e6562ee10a28a2fa7',
 'sources/primitive-slices/native/schedule_defer_return.sts': '47585f73860a5d5b7c49a4b712ca7d480c5fa50208b8f61622a98c1813288c63',
 'sources/primitive-slices/native/schedule_direct.sts': 'd48956e0334bb398e0c47c634dbccd21f39724faeb91e24c849c07f2f00c104d',
 'sources/primitive-slices/native/schedule_direct_bypass_break.sts': '3c0e5d9f61cdbfb1e7e19243b7fbd7e1f3d245b01db515c8439812ea106ecc71',
 'sources/primitive-slices/native/schedule_direct_bypass_continue.sts': '760e0c0ba3c6a5e7dc97dd70a1876bd5b1ec0beebca326f753a8b1e4932b88cc',
 'sources/primitive-slices/native/schedule_direct_bypass_return.sts': '0b3de5516b152943b3f9880e3254962d08c223108ad9188f8509841300885aaa',
 'sources/primitive-slices/native/schedule_shadow_defer.sts': 'a301487cbb2fd7ef93014e0a7bfdecc98cd188209cd1d6d47f3418167978f585',
 'sources/primitive-slices/native/schedule_shadow_siblings.sts': '4c8b90db271549c97c3abd305034d84ac9c325d7e6285eb8136f833663457241',
 'sources/primitive-slices/native/zero_explicit.sts': '5c339d47f79f0fb10176e1dab6c165c9f5c38a34f8e36c458ab56e24fd631e2a',
 'sources/primitive-slices/native/zero_omitted.sts': '024755ac14c2077aca82bb7c0579a03a586d6647d4e9bac06b6d8f38d2f91341',
 'sources/primitive-slices/reject/append-called.sts': '455dcbc5c44226a541c53e3cf432621804a183c2f582a5113219b14efb367429',
 'sources/primitive-slices/reject/append-dead.sts': '4031f6be1b9ea3d81093d771319b1afd9478df62622ce8be51751eba197752fb',
 'sources/primitive-slices/reject/append-uncalled.sts': 'e3c35f7ff3f551d02e22f27f7d6139ab227b1878dfd6b6bd67d8a610a270f36e',
 'sources/primitive-slices/reject/assignment_descriptor_query-called.sts': '106d56c152b8544bcbeb4fcfaed5507a6a5fedf9628ce038ba01338dcc93993d',
 'sources/primitive-slices/reject/assignment_descriptor_query-dead.sts': '6cac36517a287384a83451c7c9f0e93a48422ee6ebaf46396d5f0659229335d1',
 'sources/primitive-slices/reject/assignment_descriptor_query-uncalled.sts': '5fd45ef802e12b4fa4a8c5ececfb14ff0279dd54f0b6116319e67396923db4b7',
 'sources/primitive-slices/reject/captured_use-called.sts': '63a09ac63e8544e8484d614d44636631ce4539f4aaab62757ee3149cd228022a',
 'sources/primitive-slices/reject/captured_use-dead.sts': 'f30450cdb811c6c228efa3e701a35635a311dc4275a8bdf89455cdbe0453f617',
 'sources/primitive-slices/reject/captured_use-uncalled.sts': 'e569ff60036a407c865938a96b160a38b0636bbeccc4e80127626908d83896de',
 'sources/primitive-slices/reject/composite_element-called.sts': 'ae19af8880c1eb1f916caaa3d86b00cd4f015729eb9d31201a29e439f6aeec81',
 'sources/primitive-slices/reject/composite_element-dead.sts': 'ae60c6b0e17149c62ec4a9b7d3bc529e1bf40924a78d129a64afdebb2b03a25f',
 'sources/primitive-slices/reject/composite_element-uncalled.sts': '869c422f6d8cae6fc4663ab52c359547a885affd936326acde2de7cf2b4bf9ea',
 'sources/primitive-slices/reject/compound_element-called.sts': 'a40a73ad445d12b8fb1cdc84cf956980d4c94f60b2b684fb09417a0fe7d93b3b',
 'sources/primitive-slices/reject/compound_element-dead.sts': '566d148492f2ff4492a3c1a2756cba253a23f1af7d45bbc293d8df1bc95e2085',
 'sources/primitive-slices/reject/compound_element-uncalled.sts': '7a038a4e12679095edcb24202de0664974fe890337794281d5080647ec1e5a9c',
 'sources/primitive-slices/reject/conditional_descriptor_query-called.sts': '2cab9b90e5e8cb08b78053e33f65d1c5938c2387a91dfd5974d088374d5c5af1',
 'sources/primitive-slices/reject/conditional_descriptor_query-dead.sts': '1cab013e582ecf357095ae6c91486f43f54ca904f23115540c20b63b1fce6c65',
 'sources/primitive-slices/reject/conditional_descriptor_query-uncalled.sts': '491c45ed6f7cd9e88dbe91e50a27819a1e22e2832a448bc23519ea7c8ea2402d',
 'sources/primitive-slices/reject/conditional_release-called.sts': '8efc35cbece9e56c85d94c8b8c1a70487b5f8793ec138dd14e24fa2ca3c34cdd',
 'sources/primitive-slices/reject/conditional_release-dead.sts': '28f9040634d280a9892029738f691cc2405eff36ffad556567f6908cd23083af',
 'sources/primitive-slices/reject/conditional_release-uncalled.sts': 'efcb256076cf99d159553116e543d70f047058045dbb5e07221cd170babd08b3',
 'sources/primitive-slices/reject/copy-called.sts': 'ec29d4085b01c2f5838e0d5425a3af61a175a29f96a71686011821154f1dd15b',
 'sources/primitive-slices/reject/copy-dead.sts': 'ed366466244b7946e0fcfe52661c76602731aa5a4f0a28d926e9d5eb47fb7246',
 'sources/primitive-slices/reject/copy-uncalled.sts': 'c0fe7f792b37f42471ed45290eea4cec5eb8b38f440c29558e8cb88705c6f9b4',
 'sources/primitive-slices/reject/defer_extra_statement-called.sts': '36bedd33fc37a6df4e68f687824e6cc5404faa7131e17759468de7308b101d32',
 'sources/primitive-slices/reject/defer_extra_statement-dead.sts': '27eb3ec41b1ca3933b7c2aff9b6ec1d62509448eb9033ab3224c741c331afc03',
 'sources/primitive-slices/reject/defer_extra_statement-uncalled.sts': 'eae6d42e3599d3824f0d3450b3166c18b540f3147e62897f9173380d109b2eea',
 'sources/primitive-slices/reject/defer_single_statement-called.sts': 'a3bd9495db84c362fc9e99d9d8dfd50d5c4c65470cd4700a6a281321a2a4299d',
 'sources/primitive-slices/reject/defer_single_statement-dead.sts': '3473fc15810e2e8f74f764d1c29818bc2dc37f663b6b64ef2b9d5a4bd64855df',
 'sources/primitive-slices/reject/defer_single_statement-uncalled.sts': '3f6e6abba7e24debaf6f5f1e22a8a451b90dcece2251191e4c0210efd23c6d5b',
 'sources/primitive-slices/reject/deferred_constructor-called.sts': 'd87274d29da8c2669d6254f8eac0110b2e3a8cdb777aad4476c1a6a0bc236174',
 'sources/primitive-slices/reject/deferred_constructor-dead.sts': 'b702847ad370e20257bee2f514a1bd2a0fd2d298da6a1ddcdefa14fb62ff01ab',
 'sources/primitive-slices/reject/deferred_constructor-uncalled.sts': '87391be5b885f86ccb6260f3e49ae332f579aa3fb2d9e4e8c71f1e905b90a8ac',
 'sources/primitive-slices/reject/deferred_read-called.sts': '9f8799d667785f699f681a95dbf63da5fe28711cdc5cf3e913c35a8806663e60',
 'sources/primitive-slices/reject/deferred_read-dead.sts': 'd9418225e642d33373fbe24d841f57d8ea63f7dad65c54d554175ad3d3a9d6f1',
 'sources/primitive-slices/reject/deferred_read-uncalled.sts': '32082165ba5a1a3521b85ca6e4adf1992a0a6fb40b86a0dcb0809812f586467a',
 'sources/primitive-slices/reject/delayed_defer-called.sts': '8c0332595228d94ccd545aaab2eb265b796bbc3ef3a0b7b8c8e8bfb5d16e8070',
 'sources/primitive-slices/reject/delayed_defer-dead.sts': '50fe40f95c65da934ac2327f0063d5d24bb9578cbb0ac6c3d054fdd23a78a7ac',
 'sources/primitive-slices/reject/delayed_defer-uncalled.sts': '38fa628ed70dc0e63f6a3432a6c85217ac4eadab08425038f8148f68f7c9e49a',
 'sources/primitive-slices/reject/descriptor_address-called.sts': '3b2efd873ebd3d6c718cf8492525fbcb284fa37dc23744c28814a3e30d130b47',
 'sources/primitive-slices/reject/descriptor_address-dead.sts': '03df6764f4c39aa3df8216640295bdca9d8908d5acd9467f33bd0390219fa360',
 'sources/primitive-slices/reject/descriptor_address-uncalled.sts': '78e3902b6e887dbce796bee6792538dbd192c2509324c1d76e07339607078399',
 'sources/primitive-slices/reject/descriptor_argument-called.sts': '6f9c5a07e1c18f5f911196d178c7149ff879f9086c20e5b158f71619cfc5fb8d',
 'sources/primitive-slices/reject/descriptor_argument-dead.sts': '03270f2a1d20e6f2fda0d5fef4b336aa290cfb05ba39d48e97d7ec3a384d0b07',
 'sources/primitive-slices/reject/descriptor_argument-uncalled.sts': 'fd69e7f1935dbc382af53121db77815c6fb928774d1f0bee7af1e427d9e887d1',
 'sources/primitive-slices/reject/descriptor_assignment-called.sts': 'd16a9db2329640fbc39af098193224421e7fce5d5d6295b623aa84d283ae2097',
 'sources/primitive-slices/reject/descriptor_assignment-dead.sts': '0d0b0c7fd7fd893b73d130e771346b6ace5ded26bcb1f906e0ac1c1d78297bd8',
 'sources/primitive-slices/reject/descriptor_assignment-uncalled.sts': '0926f343dee7b2c2d6b3ec6a844ce72b788ab674d16108b6cebf3422b2a81798',
 'sources/primitive-slices/reject/descriptor_cast-called.sts': '8bd2bcdaf4645ef52599c6e2fa896d8a4013621e5a6072e96688d7b6b78634a9',
 'sources/primitive-slices/reject/descriptor_cast-dead.sts': '721d98456fde7342f8e4b29438b1af5b06750e089268c4e8044316d6feb9f752',
 'sources/primitive-slices/reject/descriptor_cast-uncalled.sts': 'e9da545ee17749e74c03b947c457fee77a7d8811094a802bb3fa77d19d5f32e9',
 'sources/primitive-slices/reject/descriptor_copy-called.sts': '282810d90769173298c0aeca0b86c9842e2be1a9dfa9144acd7d087fb7de51a6',
 'sources/primitive-slices/reject/descriptor_copy-dead.sts': '3f1648de11983f84b235cf5ba2d47fe4af4919be6c860ef39e404ac82ff579d6',
 'sources/primitive-slices/reject/descriptor_copy-uncalled.sts': 'b22cda9926f7b720b17adadada24bed7c49e85429902e54aece8c391766c1293',
 'sources/primitive-slices/reject/descriptor_result-called.sts': '85e12dff272aff5d2bd80f14429b843573e355b2999a53f8e5a5ca6e0803e103',
 'sources/primitive-slices/reject/descriptor_result-dead.sts': 'dd265ce55106ecdf3de355bec36225d227dab8e9a9f2decdd89c8bc6aeb4da21',
 'sources/primitive-slices/reject/descriptor_result-uncalled.sts': '916a526a1e0b28e02d755e4eb36fe3748a6d3a96feb992c57952d5e64e51b4d2',
 'sources/primitive-slices/reject/direct_with_deferred_read-called.sts': '515c4adcf241a56eb1580c2f385348506a90672df108bd50c69962e5d87ba2b2',
 'sources/primitive-slices/reject/direct_with_deferred_read-dead.sts': 'fbfe8879d7480b1e3598db1fb69cbf9e0ec4f4bc5f59f24433219e94ba08a0c6',
 'sources/primitive-slices/reject/direct_with_deferred_read-uncalled.sts': 'e3798de50fcb2db77f1e9aeb2152363cc40095603657c063348b22e63b964ba3',
 'sources/primitive-slices/reject/discarded_declaration-called.sts': 'b5fc59a0c8adbef47fc8c25daf2c2be47b0a12be4389f03ce8563b83a8aacb68',
 'sources/primitive-slices/reject/discarded_declaration-dead.sts': 'c5ad94bc7ba071b23affc3cb1ad61e8af333e39d0fdbb93ed01e762690e40c39',
 'sources/primitive-slices/reject/discarded_declaration-uncalled.sts': 'e88498d5cf155e4c75f515cabe9b6a3276c856d9bcc6efd789a0642136e182de',
 'sources/primitive-slices/reject/double_release-called.sts': 'b0f0d2cc65fc5007be0234a688c88e2436cf21ae0f4937a12fe2c9aaeb18c3c2',
 'sources/primitive-slices/reject/double_release-dead.sts': '33072e1302e794f1d99ac7e3154c2cd072e8b64da095264a3bedb9b4c3d6f58a',
 'sources/primitive-slices/reject/double_release-uncalled.sts': '5f93a4e2a515530a9acc40b970ec5a38e2c6bd4ce0512e85ae2811b3978117dd',
 'sources/primitive-slices/reject/element_address-called.sts': '3a6d38db13c11bd6a3a1a9e95ceb656a6946f167fb2dcda0896123fd6e81edf2',
 'sources/primitive-slices/reject/element_address-dead.sts': 'b359b49c27abfb3d931139a7c4d0dc4f2b6adbd717efa9c551c05d50a7ada02c',
 'sources/primitive-slices/reject/element_address-uncalled.sts': '4eb9fa072540cf0bd6a7f3706130da8dc4a475092f78b7ab01fb3bd49edf18ad',
 'sources/primitive-slices/reject/element_alias-called.sts': '03c29b6158886dc3918437330905b5130348f4c03f506cb9f17740cd0b377713',
 'sources/primitive-slices/reject/element_alias-dead.sts': 'f64d80c030d24affb0b176646d4129136241ab100026738dd18c598229e5c34a',
 'sources/primitive-slices/reject/element_alias-uncalled.sts': '8b31458eeea0830c08ba10640b070bc121f8f7196cb4e601cb5a0025db85dbbb',
 'sources/primitive-slices/reject/fat_arrow_constructor-called.sts': 'f86c8da13432be6ad168fd4b951abd58fe510177905908b2e28fee64bfbbfd82',
 'sources/primitive-slices/reject/fat_arrow_constructor-dead.sts': '1a1b284ee45fd65966a378f458ce6dd1e76e611e653b0f0c1695a4ac6f231719',
 'sources/primitive-slices/reject/fat_arrow_constructor-uncalled.sts': '24d0cd68a0cc2220874700fb4bfb011ba590622dfc9c945bb87c920f0596c24f',
 'sources/primitive-slices/reject/float_count-called.sts': 'b075adb342d7d1970da9148f34e0d135c3fda5d8b1375f2a3d0cbe6345d9feca',
 'sources/primitive-slices/reject/float_count-dead.sts': '26ded7ae325c2d37f8b9d77514b511f6e2ac29d6de749c0987b4472359c6be46',
 'sources/primitive-slices/reject/float_count-uncalled.sts': '92dc88ca0e8ca8d206ea3523652127df96e9524ac64bc04373072360f6609171',
 'sources/primitive-slices/reject/for_initializer-called.sts': '22dbdfd552c64046d592c7a856c1c7aae55352d86eade14052e15f8b144ec5d1',
 'sources/primitive-slices/reject/for_initializer-dead.sts': '0294ba059d70b3c904bea819ad3267b457f96f64852372ef91ca0eb11c1c93f3',
 'sources/primitive-slices/reject/for_initializer-uncalled.sts': 'f66067ecb0a44f68e044629f7ef7c3c121d51d11af1b36bc293485b33278aed8',
 'sources/primitive-slices/reject/foreach-called.sts': 'df6cee34495b35dc89ae77172f99c60f69a8ad07f53d850d862138635b0a0725',
 'sources/primitive-slices/reject/foreach-dead.sts': '4db74d4ffdb35ab3250a4076123fded483371e1fc0640dc2c3792c160d52f6e0',
 'sources/primitive-slices/reject/foreach-uncalled.sts': 'a7dee02ad4dce1761742c74b4d9b12f42c94525caf3542f02b977fed63c0719c',
 'sources/primitive-slices/reject/generic_constructor-called.sts': 'b7756ed61d67eecbdaefe614cc3bae143277a9921ce583b3ab83b0c624d4a0cb',
 'sources/primitive-slices/reject/generic_constructor-dead.sts': '712386be581b16eead867ac17a24c83fa57d0c7ce28e467aff64e971f0e4dea8',
 'sources/primitive-slices/reject/generic_constructor-uncalled.sts': 'fce3a8db386923426e785d6c50a3c362f9ce5ff4a33db41cf286f37642e48a7a',
 'sources/primitive-slices/reject/global_slice.sts': '76e4ad80915fec040003113c17cd061e16b296089d502ffd6574bd295b3b9a3d',
 'sources/primitive-slices/reject/inferred_declaration-called.sts': '0a489acbd9098b72b1591cba7e1a9c860c0bf4a5efd58f592fbeac0989bc6efe',
 'sources/primitive-slices/reject/inferred_declaration-dead.sts': 'fe901c044d6f6a0172b95426bdadb775ad4556ec5ba1c6d28a54a6a594a07be2',
 'sources/primitive-slices/reject/inferred_declaration-uncalled.sts': 'd450e8926934b713830b11c6a3e0774e17dfcac074dbdd47ec16c257e3b5f956',
 'sources/primitive-slices/reject/inline_make-called.sts': 'a646bc9819930ebb3ce34c98055d5d8fb1ed79e1ad4deffb0b808dd17be31622',
 'sources/primitive-slices/reject/inline_make-dead.sts': '1beae924997e0072fe390f3225b4766d3f94a8e5ae9e92b8b0e236f89a8e4955',
 'sources/primitive-slices/reject/inline_make-uncalled.sts': 'f1d618a7d08d69f7f3f9b77c8c7c9a7594b4bd583048b00a726bab82c2fd3074',
 'sources/primitive-slices/reject/loop_release_outer-called.sts': '2274684491aaa60b333925bb275d528b828c0b5f87edc3ba123a9d6cc8b88870',
 'sources/primitive-slices/reject/loop_release_outer-dead.sts': '325d9067d45817b367ea057f6fc38b31352a0bd4662841534ae66c9a7ea47d40',
 'sources/primitive-slices/reject/loop_release_outer-uncalled.sts': '6160179001b9548744f1897e450cb77a510edc454c7d945f54c7e3c2b1a87385',
 'sources/primitive-slices/reject/member_query-called.sts': '045f9c2f46fb7cc19f5c2f46169e0329dca85432a18d8d331ffeec35d346be48',
 'sources/primitive-slices/reject/member_query-dead.sts': '45c49bac8eefa03532c313a91d306cec9509ec6f7661b430f011c5208c64947f',
 'sources/primitive-slices/reject/member_query-uncalled.sts': '26ed9931684a56f1f85f1a47980b4b1a570072709fd5c5ceffcb33fd0da373ed',
 'sources/primitive-slices/reject/nested_constructor-called.sts': '6dc62df7a87fe78d5e8febaf81114bb77b550100c92003400a392b29f2f94746',
 'sources/primitive-slices/reject/nested_constructor-dead.sts': '9c4f359216a6f946100db061b892b1ed7ce52d0cb86bb9dd668fbdfc13bb59a3',
 'sources/primitive-slices/reject/nested_constructor-uncalled.sts': 'f2170dd2a0483604abae14444a7f2c636ad02a126a91a33b2d03c009f0e976a5',
 'sources/primitive-slices/reject/nested_element_address-called.sts': '58fb295fb7a3ffda2faf9efe114c777b3441b5e3ac8ec9a55dd8a4518b10a671',
 'sources/primitive-slices/reject/nested_element_address-dead.sts': '26684cd9ba63656a8f96924b135c996997676387bb21ecb54514382489071d6e',
 'sources/primitive-slices/reject/nested_element_address-uncalled.sts': '03c7ee79d620908edede52b3fe622e3a5f14a4c8e556ee32a3cd50c1b370be92',
 'sources/primitive-slices/reject/nested_element_type-called.sts': 'df505f14f0df09f3973234fe6f03d78bbe4af7b35455b6e62f97d4c505ecf79a',
 'sources/primitive-slices/reject/nested_element_type-dead.sts': '8eb995d7c5680a724948d323d210f880d99cde2f7ac7ffdabcf46ddfec0266f4',
 'sources/primitive-slices/reject/nested_element_type-uncalled.sts': '8ea297b1145c07ed97c87a8995fe488de6c81be62bec7a69a21d8aa4b82a5d30',
 'sources/primitive-slices/reject/nested_release-called.sts': 'b32f93a74a950061e110371cfca8a14c62d094981a144c4aa224f987cc149f74',
 'sources/primitive-slices/reject/nested_release-dead.sts': '042e40fa5a965f95882f8901fe0ce30700618b7cc1c68d337d7ae611d6f9977e',
 'sources/primitive-slices/reject/nested_release-uncalled.sts': '67566b1645ba319a66ef3fd1568143e226fc58db3e88be74a39df954619326b2',
 'sources/primitive-slices/reject/nil_declaration-called.sts': '3b2050e47400ba6f7cf3e3f60479d026fbcfd56e78a9939a5870d363b7d5a025',
 'sources/primitive-slices/reject/nil_declaration-dead.sts': '1e27899847b996f64f4732c383692c02eae78a4cda6450156c4cdd26de78fdeb',
 'sources/primitive-slices/reject/nil_declaration-uncalled.sts': 'bad8a38f402b7a01c36e4efb99954d23b4be0dc3e765f15fc9bee666b2193b21',
 'sources/primitive-slices/reject/pointer_count-called.sts': 'ac927a08c9951b1a5d08e60d706b9f71ed44825b477d543eb50850fa4f8c984c',
 'sources/primitive-slices/reject/pointer_count-dead.sts': '74ee08c2fa1a0cd228c637f85e22f720361f51c0e3b4d0f8dc72f989e02a8b46',
 'sources/primitive-slices/reject/pointer_count-uncalled.sts': '784dcce3767e13ead80460835b07f3d90a435423b516814392fa8503d1407196',
 'sources/primitive-slices/reject/pointer_element_type-called.sts': '974d43f81a79c2d0cbb576da7ed3453e070acaa83a3c4633a74ec8cac4b45b96',
 'sources/primitive-slices/reject/pointer_element_type-dead.sts': '2cb39bb635b28c434a0b10a336cd4acc9c7e54d4fff9f3ce6d8e7ff70fd84732',
 'sources/primitive-slices/reject/pointer_element_type-uncalled.sts': 'e132f9e6b0b1773c0116dce586ec07c2e37c3f20e08d08a8cd89320b7384591e',
 'sources/primitive-slices/reject/postfix_element-called.sts': '2cb548a700dcb57e3babe09729f5ba343ed2c94526d5360f3a79bf4a103e5523',
 'sources/primitive-slices/reject/postfix_element-dead.sts': '124fe35633cf4f62e0c09271a3bca831c538b54c2c157ca8f85dfefd9c505c18',
 'sources/primitive-slices/reject/postfix_element-uncalled.sts': 'ecf4cf371b2b6b8773aa0894fb09262978c3bba285b840532d8d1d6ec8855bda',
 'sources/primitive-slices/reject/qualified_declaration-called.sts': '875882dae4a6336e3c06c03d886d72719e3c8d807451cecc900ee4f96811a981',
 'sources/primitive-slices/reject/qualified_declaration-dead.sts': 'eb8000aaa878337ed77f511148d197b76584d75bf8664004cd2ab738842e6fd2',
 'sources/primitive-slices/reject/qualified_declaration-uncalled.sts': '009e5644519d4c01ca2bab1186e825943ffcb54592b1400b0d73da2011fa58ef',
 'sources/primitive-slices/reject/query_descriptor_call.sts': '68c03f9f3d70516f7eda1c3374086d0b74125f595d88a82949136346cf6526bf',
 'sources/primitive-slices/reject/query_raw_pointer.sts': '75326cb51a1f737e4b2ccf0d8552cc6d106365c80c53517466da96f9876139bf',
 'sources/primitive-slices/reject/release_expression-called.sts': '8118f48f0a1cf6b7368ba2a013748748732ccefbec2a39ededd2677683f12b86',
 'sources/primitive-slices/reject/release_expression-dead.sts': '68b2a79c1838dcb69a0c03656b37cefd8c0b6cb2c361842d29ba0d62347eab82',
 'sources/primitive-slices/reject/release_expression-uncalled.sts': 'b96759b50676da8e1a87985cb1f640538285f21ee9356f18a0c3f48ad8221c83',
 'sources/primitive-slices/reject/release_plus_defer-called.sts': '0570fdd423f2b3b2036fee77e4a96a4f62ab1ec297780bf46c1b996e2c3489d8',
 'sources/primitive-slices/reject/release_plus_defer-dead.sts': '8c844547002ee805bef6329a87335eb49006c80b0588f08ba6957f16ccd5baf9',
 'sources/primitive-slices/reject/release_plus_defer-uncalled.sts': '0549077821014e9f2ee29b0066585f0d29fec5720edf602204d6e8e65856fb86',
 'sources/primitive-slices/reject/release_then_nested_use-called.sts': '731aa97b6a05b6e8727d356f83efebe37d785dbcb352ff2f08dd2d71b1519a00',
 'sources/primitive-slices/reject/release_then_nested_use-dead.sts': '076a64aec70e8cccba03af70e96f9c5d293b0da03373daa3190e5a3cbd3bf9c8',
 'sources/primitive-slices/reject/release_then_nested_use-uncalled.sts': '7da558ea887f1c1b37fc3d45e176e3faed7e783315ba7e259277a1c023388a6d',
 'sources/primitive-slices/reject/release_then_use-called.sts': 'f169dc7eb31ebf5ad086065879ee003987be4f210eb65587301b8d1beb1af2c1',
 'sources/primitive-slices/reject/release_then_use-dead.sts': '461f0a91b7e422721ca4788bb0dccb87393cb08bdf71d9af5e240286cdcaf37d',
 'sources/primitive-slices/reject/release_then_use-uncalled.sts': '8abe8f22623c3e45693982606a31be645c3013d37a8a7069fce16347b62e2a2b',
 'sources/primitive-slices/reject/slice_array-called.sts': '39f7c0222f9bff0517288af36c538e5debe6124cfda6e6ce0ee9daec6d78545f',
 'sources/primitive-slices/reject/slice_array-dead.sts': 'dc5a06a01edaa00b9634844b1f3ab8aa4d7d5a3b65eab3fa584d559393d15efc',
 'sources/primitive-slices/reject/slice_array-uncalled.sts': 'be73202141f96b9ce1e28a1b697642b9f17855e6a18554cc5119081e2bddc426',
 'sources/primitive-slices/reject/stack_declaration-called.sts': 'a7a5e6ddcc9ab73ddeb0dda540e9b938758a03d4307e44d5466033a0336b3940',
 'sources/primitive-slices/reject/stack_declaration-dead.sts': '9c794bcda6af656e6628f798bdd8c7d0a4ceb8d82265f0ab90ca5d8c79b73bb0',
 'sources/primitive-slices/reject/stack_declaration-uncalled.sts': 'f34699fe93b6b9fe318f62787e61d34ce7250bfc57c7275d4dae3b5d758e59d0',
 'sources/primitive-slices/reject/synthetic_match_binding-called.sts': '82eb5b14486e54cb91436238497c608425fe53caece22097f68099dad96bcfd2',
 'sources/primitive-slices/reject/synthetic_match_binding-dead.sts': '6bfcde90633ed75e1726e699abaf3aa9d2f7da0758878980f5a9b77f2eb5f081',
 'sources/primitive-slices/reject/synthetic_match_binding-uncalled.sts': '6c9b9b80ffa93af806c7dfd3297e4f9b4009a7a3f5478a282e5403b516c44c66',
 'sources/primitive-slices/reject/unchecked_slice_use-called.sts': '9fd0975f4474d89fa70e70eb15c7e17fcf4297e614e61d3236bdafcdece76029',
 'sources/primitive-slices/reject/unchecked_slice_use-dead.sts': 'e19e53d84c1339836a72a39591fc414349a8e47dc39930e47c153c4fd15c2c61',
 'sources/primitive-slices/reject/unchecked_slice_use-uncalled.sts': 'fd0cfb22630aaca6c9b7b19ea4fb7bc03b42fa0ce126b32fe06aac56e09cace8',
 'sources/primitive-slices/reject/uninitialized-called.sts': '1527806cac28de6f83d611cda669bca40ebba61ca20d28a5cccd106eb18899b5',
 'sources/primitive-slices/reject/uninitialized-dead.sts': '062cd4fff42e4b210fc1d4f2ad89840a33dc9193c5732189abd306124e6e4c9f',
 'sources/primitive-slices/reject/uninitialized-uncalled.sts': 'dcf274419e0288db703856e07c3afd35ecc3e8e20c9f8f938efc026066c711fc',
 'sources/primitive-slices/reject/unsafe_slice_use-called.sts': '0393a6eb451dfa97caeeef7971dc182270b6fb0cb9dc12a76bb71fe5f176465e',
 'sources/primitive-slices/reject/unsafe_slice_use-dead.sts': '622339f8a8ed55e072463cf3b377fe565653ebda0fde6422ca430a8e54a4c753',
 'sources/primitive-slices/reject/unsafe_slice_use-uncalled.sts': '1a4d9a41d3e1d9d1087b4db4ee22e2fe96d6ccf46c3f9c712008c13fcf54ba40',
 'sources/primitive-slices/reject/view-called.sts': 'a2d00f967f9d8a97438a92509beae7bcc4dd28f2970bcdcc723784dd8905b946',
 'sources/primitive-slices/reject/view-dead.sts': 'e4afd8dafdf159f585af06598bbc9daaae06cfe2a122414d48e1cd1c352f5448',
 'sources/primitive-slices/reject/view-uncalled.sts': '3c54cb90ff4e7f21abf302b07e6e8a70678a30e81050c429135165d89b1ea8b5',
 'sources/primitive-slices/reject/whole_assertion-called.sts': 'a9d0b07910ea335c16b445f4d3ed20012a3f097635d93286ab1e74a474eef89b',
 'sources/primitive-slices/reject/whole_assertion-dead.sts': '42704daba73c1a87507b796d44a56133312260d7440ee51190d30037b9633693',
 'sources/primitive-slices/reject/whole_assertion-uncalled.sts': '3d03329ffe124bfbd599f75702584c5460519d6b86d5480550d575a0207d4e1b',
 'sources/primitive-slices/reject/whole_comparison-called.sts': '6f6d473480ebe73608c5ded14a841142c9f031599880842fcd8f0f4c03915dfc',
 'sources/primitive-slices/reject/whole_comparison-dead.sts': '0f5e368e8a036138f32345284c8d15e0ea7c5b2b8a4d139b6888eb4612e10918',
 'sources/primitive-slices/reject/whole_comparison-uncalled.sts': '45ce45db18c08f54aedee271d2bf4cd4916413a8e90621da1c59e78a645d6811',
 'sources/primitive-slices/reject/whole_expression-called.sts': 'fca4ad3e81927c87be080e36d9e67632343ee4745660fbc2b7d7c36b301d7efe',
 'sources/primitive-slices/reject/whole_expression-dead.sts': 'f4486cd59b7a7b2adaf7ec2df9efb4435931c4b24e172eaa2f9e5b65e01ffead',
 'sources/primitive-slices/reject/whole_expression-uncalled.sts': '6b6571baacf8d27087df274b1401f46ff7417b9008e11b9b169aece1b3d0e4db',
 'sources/primitive-slices/reject/whole_print-called.sts': '1bdf752fcb25205406cc0aeedd4a83c988386d01460a4428a1a4d84955e84e75',
 'sources/primitive-slices/reject/whole_print-dead.sts': '89d4c7941817eca5882d24375db445e8e0d9298b1b7d8d82cc96f0767cf57822',
 'sources/primitive-slices/reject/whole_print-uncalled.sts': 'fedb18c562b91745f570789910990aa1aba62e6a7c7bf6195e7abca4b10a76a6',
 'sources/primitive-slices/reject/wrong_element_identity-called.sts': '24869e33076de74a02d411b985843c9e1fcbbc9abc8715bff5c6e4fba362de56',
 'sources/primitive-slices/reject/wrong_element_identity-dead.sts': '54beb5443126fe6afcd4fb53a0d4b859259b0e3100a2283053c1711fc8fc71e1',
 'sources/primitive-slices/reject/wrong_element_identity-uncalled.sts': '0b83ac648ed89980f7c285ec3092958ad8f633025374491524e3f52ab2d8cde9'}

GENERATED_SOURCES = {'sources/primitive-slices/count-failure/cap_i16_neg.sts': 'mod slice_case;\n'
                                                           "test 'case' {\n"
                                                           '    stack i16 n = -1; heap []i32 s = make.([]i32, 0, n);\n'
                                                           '}\n',
 'sources/primitive-slices/count-failure/cap_i32_neg.sts': 'mod slice_case;\n'
                                                           "test 'case' {\n"
                                                           '    stack i32 n = -1; heap []i32 s = make.([]i32, 0, n);\n'
                                                           '}\n',
 'sources/primitive-slices/count-failure/cap_i64_neg.sts': 'mod slice_case;\n'
                                                           "test 'case' {\n"
                                                           '    stack i64 n = -1; heap []i32 s = make.([]i32, 0, n);\n'
                                                           '}\n',
 'sources/primitive-slices/count-failure/cap_i64_over.sts': 'mod slice_case;\n'
                                                            "test 'case' {\n"
                                                            '    stack i64 n = 2147483648; heap []i32 s = make.([]i32, 0, n);\n'
                                                            '}\n',
 'sources/primitive-slices/count-failure/cap_i8_neg.sts': 'mod slice_case;\n'
                                                          "test 'case' {\n"
                                                          '    stack i8 n = -1; heap []i32 s = make.([]i32, 0, n);\n'
                                                          '}\n',
 'sources/primitive-slices/count-failure/cap_u32_max.sts': 'mod slice_case;\n'
                                                           "test 'case' {\n"
                                                           '    stack u32 n = 4294967295; heap []i32 s = make.([]i32, 0, n);\n'
                                                           '}\n',
 'sources/primitive-slices/count-failure/cap_u32_over.sts': 'mod slice_case;\n'
                                                            "test 'case' {\n"
                                                            '    stack u32 n = 2147483648; heap []i32 s = make.([]i32, 0, n);\n'
                                                            '}\n',
 'sources/primitive-slices/count-failure/cap_u64_max.sts': 'mod slice_case;\n'
                                                           "test 'case' {\n"
                                                           '    stack u64 n = 18446744073709551615; heap []i32 s = make.([]i32, '
                                                           '0, n);\n'
                                                           '}\n',
 'sources/primitive-slices/count-failure/cap_u64_wrap.sts': 'mod slice_case;\n'
                                                            "test 'case' {\n"
                                                            '    stack u64 n = 4294967296; heap []i32 s = make.([]i32, 0, n);\n'
                                                            '}\n',
 'sources/primitive-slices/count-failure/cap_u64_wrap1.sts': 'mod slice_case;\n'
                                                             "test 'case' {\n"
                                                             '    stack u64 n = 4294967297; heap []i32 s = make.([]i32, 0, n);\n'
                                                             '}\n',
 'sources/primitive-slices/count-failure/invalid_order.sts': 'mod observation_slice_order;\n'
                                                             'stack i32 trace = 0;\n'
                                                             'fn step(stack i32 tag, stack i32 value): i32 {\n'
                                                             '    trace = trace * 10 + tag;\n'
                                                             '    ret value;\n'
                                                             '}\n'
                                                             'fn exercise(void): void {\n'
                                                             '    heap []i32 s = make.([]i32, step(1, -1), step(2, 4));\n'
                                                             '    expect_eq.(trace, 12);\n'
                                                             '    rem.(s);\n'
                                                             '    trace = 0;\n'
                                                             '    heap []i16 t = make.([]i16, step(1, 3));\n'
                                                             '    expect_eq.(trace, 1);\n'
                                                             '    rem.(t);\n'
                                                             '    trace = 0;\n'
                                                             '    heap []i32 z = make.([]i32, step(1, 0), step(2, 0));\n'
                                                             '    expect_eq.(trace, 12);\n'
                                                             '    rem.(z);\n'
                                                             '}\n'
                                                             "test 'count order' { exercise(); }\n",
 'sources/primitive-slices/count-failure/len_gt_cap.sts': 'mod slice_case;\n'
                                                          "test 'case' {\n"
                                                          '    heap []i32 s = make.([]i32, 3, 2);\n'
                                                          '}\n',
 'sources/primitive-slices/count-failure/len_i16_neg.sts': 'mod slice_case;\n'
                                                           "test 'case' {\n"
                                                           '    stack i16 n = -1; heap []i32 s = make.([]i32, n);\n'
                                                           '}\n',
 'sources/primitive-slices/count-failure/len_i32_neg.sts': 'mod slice_case;\n'
                                                           "test 'case' {\n"
                                                           '    stack i32 n = -1; heap []i32 s = make.([]i32, n);\n'
                                                           '}\n',
 'sources/primitive-slices/count-failure/len_i64_neg.sts': 'mod slice_case;\n'
                                                           "test 'case' {\n"
                                                           '    stack i64 n = -1; heap []i32 s = make.([]i32, n);\n'
                                                           '}\n',
 'sources/primitive-slices/count-failure/len_i64_over.sts': 'mod slice_case;\n'
                                                            "test 'case' {\n"
                                                            '    stack i64 n = 2147483648; heap []i32 s = make.([]i32, n);\n'
                                                            '}\n',
 'sources/primitive-slices/count-failure/len_i8_neg.sts': 'mod slice_case;\n'
                                                          "test 'case' {\n"
                                                          '    stack i8 n = -1; heap []i32 s = make.([]i32, n);\n'
                                                          '}\n',
 'sources/primitive-slices/count-failure/len_u32_max.sts': 'mod slice_case;\n'
                                                           "test 'case' {\n"
                                                           '    stack u32 n = 4294967295; heap []i32 s = make.([]i32, n);\n'
                                                           '}\n',
 'sources/primitive-slices/count-failure/len_u32_over.sts': 'mod slice_case;\n'
                                                            "test 'case' {\n"
                                                            '    stack u32 n = 2147483648; heap []i32 s = make.([]i32, n);\n'
                                                            '}\n',
 'sources/primitive-slices/count-failure/len_u64_max.sts': 'mod slice_case;\n'
                                                           "test 'case' {\n"
                                                           '    stack u64 n = 18446744073709551615; heap []i32 s = make.([]i32, '
                                                           'n);\n'
                                                           '}\n',
 'sources/primitive-slices/count-failure/len_u64_wrap.sts': 'mod slice_case;\n'
                                                            "test 'case' {\n"
                                                            '    stack u64 n = 4294967296; heap []i32 s = make.([]i32, n);\n'
                                                            '}\n',
 'sources/primitive-slices/count-failure/len_u64_wrap1.sts': 'mod slice_case;\n'
                                                             "test 'case' {\n"
                                                             '    stack u64 n = 4294967297; heap []i32 s = make.([]i32, n);\n'
                                                             '}\n',
 'sources/primitive-slices/native/count_bool.sts': 'mod slice_case;\n'
                                                   "test 'case' {\n"
                                                   '    stack bool n = true; heap []i32 s = make.([]i32, n); expect_eq.(len.(s), '
                                                   '1); expect_eq.(cap.(s), 1); expect_eq.(s[0], 0); rem.(s);\n'
                                                   '}\n',
 'sources/primitive-slices/native/count_i16.sts': 'mod slice_case;\n'
                                                  "test 'case' {\n"
                                                  '    stack i16 n = 1; heap []i32 s = make.([]i32, n); expect_eq.(len.(s), 1); '
                                                  'expect_eq.(cap.(s), 1); expect_eq.(s[0], 0); rem.(s);\n'
                                                  '}\n',
 'sources/primitive-slices/native/count_i32.sts': 'mod slice_case;\n'
                                                  "test 'case' {\n"
                                                  '    stack i32 n = 1; heap []i32 s = make.([]i32, n); expect_eq.(len.(s), 1); '
                                                  'expect_eq.(cap.(s), 1); expect_eq.(s[0], 0); rem.(s);\n'
                                                  '}\n',
 'sources/primitive-slices/native/count_i64.sts': 'mod slice_case;\n'
                                                  "test 'case' {\n"
                                                  '    stack i64 n = 1; heap []i32 s = make.([]i32, n); expect_eq.(len.(s), 1); '
                                                  'expect_eq.(cap.(s), 1); expect_eq.(s[0], 0); rem.(s);\n'
                                                  '}\n',
 'sources/primitive-slices/native/count_i8.sts': 'mod slice_case;\n'
                                                 "test 'case' {\n"
                                                 '    stack i8 n = 1; heap []i32 s = make.([]i32, n); expect_eq.(len.(s), 1); '
                                                 'expect_eq.(cap.(s), 1); expect_eq.(s[0], 0); rem.(s);\n'
                                                 '}\n',
 'sources/primitive-slices/native/count_u16.sts': 'mod slice_case;\n'
                                                  "test 'case' {\n"
                                                  '    stack u16 n = 1; heap []i32 s = make.([]i32, n); expect_eq.(len.(s), 1); '
                                                  'expect_eq.(cap.(s), 1); expect_eq.(s[0], 0); rem.(s);\n'
                                                  '}\n',
 'sources/primitive-slices/native/count_u32.sts': 'mod slice_case;\n'
                                                  "test 'case' {\n"
                                                  '    stack u32 n = 1; heap []i32 s = make.([]i32, n); expect_eq.(len.(s), 1); '
                                                  'expect_eq.(cap.(s), 1); expect_eq.(s[0], 0); rem.(s);\n'
                                                  '}\n',
 'sources/primitive-slices/native/count_u64.sts': 'mod slice_case;\n'
                                                  "test 'case' {\n"
                                                  '    stack u64 n = 1; heap []i32 s = make.([]i32, n); expect_eq.(len.(s), 1); '
                                                  'expect_eq.(cap.(s), 1); expect_eq.(s[0], 0); rem.(s);\n'
                                                  '}\n',
 'sources/primitive-slices/native/count_u8.sts': 'mod slice_case;\n'
                                                 "test 'case' {\n"
                                                 '    stack u8 n = 1; heap []i32 s = make.([]i32, n); expect_eq.(len.(s), 1); '
                                                 'expect_eq.(cap.(s), 1); expect_eq.(s[0], 0); rem.(s);\n'
                                                 '}\n',
 'sources/primitive-slices/native/count_unsigned_top_bit.sts': 'mod slice_case;\n'
                                                               "test 'case' {\n"
                                                               '    stack u8 n = 128; heap []i16 s = make.([]i16, n); '
                                                               'expect_eq.(len.(s), 128); expect_eq.(s[127], 0); rem.(s);\n'
                                                               '}\n',
 'sources/primitive-slices/native/mixed_both.sts': 'mod slice_case;\n'
                                                   "test 'case' {\n"
                                                   '    heap u8 *rw p = new.(7); heap []i32 s = make.([]i32, 3, 5); expect.(p != '
                                                   'nil); expect_eq.(len.(s), 3);\n'
                                                   '}\n',
 'sources/primitive-slices/native/mixed_explicit_only.sts': 'mod slice_case;\n'
                                                            "test 'case' {\n"
                                                            '    heap u8 *rw p = new.(7); heap []i32 s = make.([]i32, 3, 5); '
                                                            'expect.(p != nil); expect_eq.(len.(s), 3); rem.(s);\n'
                                                            '}\n',
 'sources/primitive-slices/native/mixed_slice_only.sts': 'mod slice_case;\n'
                                                         "test 'case' {\n"
                                                         '    heap u8 *rw p = new.(7); heap []i32 s = make.([]i32, 3, 5); '
                                                         'expect.(p != nil); expect_eq.(len.(s), 3); rem.(p);\n'
                                                         '}\n',
 'sources/primitive-slices/native/nested_scalar.sts': 'mod slice_nested;\n'
                                                      'fn scalar(stack i32 x): i32 { ret x; }\n'
                                                      'fn work(void): i32 {\n'
                                                      '    heap []i32 s = make.([]i32, 3, 5);\n'
                                                      '    defer { rem.(s); }\n'
                                                      '    s[0] = 1; s[1] = 2;\n'
                                                      '    heap []i16 t = make.([]i16, scalar(s[s[0]]), cap.(s) - 1);\n'
                                                      '    defer { rem.(t); }\n'
                                                      '    ret scalar(len.(t)) + s[s[0]];\n'
                                                      '}\n'
                                                      "test 'nested scalar' { expect_eq.(work(), 4); }\n",
 'sources/primitive-slices/native/no_assertions.sts': 'mod slice_no_assertions;\n'
                                                      'fn work(void): void {\n'
                                                      '    heap []i32 s = make.([]i32, 1, 2);\n'
                                                      '    rem.(s);\n'
                                                      '}\n'
                                                      "test 'no assertions' { work(); }\n",
 'sources/primitive-slices/native/omitted_i16.sts': 'mod slice_case;\n'
                                                    "test 'case' {\n"
                                                    '    heap []i16 s = make.([]i16, 3); expect_eq.(cap.(s), 3);\n'
                                                    '}\n',
 'sources/primitive-slices/native/primitive_bool.sts': 'mod slice_case;\n'
                                                       "test 'case' {\n"
                                                       '    heap []bool s = make.([]bool, 3, 5); expect_eq.(len.(s), 3); '
                                                       'expect_eq.(cap.(s), 5); expect_eq.(s[0], false); expect_eq.(s[2], '
                                                       'false); s[1] = true; expect_eq.(s[1], true); rem.(s);\n'
                                                       '}\n',
 'sources/primitive-slices/native/primitive_f32.sts': 'mod slice_case;\n'
                                                      "test 'case' {\n"
                                                      '    heap []f32 s = make.([]f32, 3, 5); expect_eq.(len.(s), 3); '
                                                      'expect_eq.(cap.(s), 5); expect_eq.(s[0], 0.0); expect_eq.(s[2], 0.0); '
                                                      's[1] = 1.0; expect_eq.(s[1], 1.0); rem.(s);\n'
                                                      '}\n',
 'sources/primitive-slices/native/primitive_f64.sts': 'mod slice_case;\n'
                                                      "test 'case' {\n"
                                                      '    heap []f64 s = make.([]f64, 3, 5); expect_eq.(len.(s), 3); '
                                                      'expect_eq.(cap.(s), 5); expect_eq.(s[0], 0.0); expect_eq.(s[2], 0.0); '
                                                      's[1] = 1.0; expect_eq.(s[1], 1.0); rem.(s);\n'
                                                      '}\n',
 'sources/primitive-slices/native/primitive_i16.sts': 'mod slice_case;\n'
                                                      "test 'case' {\n"
                                                      '    heap []i16 s = make.([]i16, 3, 5); expect_eq.(len.(s), 3); '
                                                      'expect_eq.(cap.(s), 5); expect_eq.(s[0], 0); expect_eq.(s[2], 0); s[1] = '
                                                      '1; expect_eq.(s[1], 1); rem.(s);\n'
                                                      '}\n',
 'sources/primitive-slices/native/primitive_i32.sts': 'mod slice_case;\n'
                                                      "test 'case' {\n"
                                                      '    heap []i32 s = make.([]i32, 3, 5); expect_eq.(len.(s), 3); '
                                                      'expect_eq.(cap.(s), 5); expect_eq.(s[0], 0); expect_eq.(s[2], 0); s[1] = '
                                                      '1; expect_eq.(s[1], 1); rem.(s);\n'
                                                      '}\n',
 'sources/primitive-slices/native/primitive_i64.sts': 'mod slice_case;\n'
                                                      "test 'case' {\n"
                                                      '    heap []i64 s = make.([]i64, 3, 5); expect_eq.(len.(s), 3); '
                                                      'expect_eq.(cap.(s), 5); expect_eq.(s[0], 0); expect_eq.(s[2], 0); s[1] = '
                                                      '1; expect_eq.(s[1], 1); rem.(s);\n'
                                                      '}\n',
 'sources/primitive-slices/native/primitive_i8.sts': 'mod slice_case;\n'
                                                     "test 'case' {\n"
                                                     '    heap []i8 s = make.([]i8, 3, 5); expect_eq.(len.(s), 3); '
                                                     'expect_eq.(cap.(s), 5); expect_eq.(s[0], 0); expect_eq.(s[2], 0); s[1] = '
                                                     '1; expect_eq.(s[1], 1); rem.(s);\n'
                                                     '}\n',
 'sources/primitive-slices/native/primitive_u16.sts': 'mod slice_case;\n'
                                                      "test 'case' {\n"
                                                      '    heap []u16 s = make.([]u16, 3, 5); expect_eq.(len.(s), 3); '
                                                      'expect_eq.(cap.(s), 5); expect_eq.(s[0], 0); expect_eq.(s[2], 0); s[1] = '
                                                      '1; expect_eq.(s[1], 1); rem.(s);\n'
                                                      '}\n',
 'sources/primitive-slices/native/primitive_u32.sts': 'mod slice_case;\n'
                                                      "test 'case' {\n"
                                                      '    heap []u32 s = make.([]u32, 3, 5); expect_eq.(len.(s), 3); '
                                                      'expect_eq.(cap.(s), 5); expect_eq.(s[0], 0); expect_eq.(s[2], 0); s[1] = '
                                                      '1; expect_eq.(s[1], 1); rem.(s);\n'
                                                      '}\n',
 'sources/primitive-slices/native/primitive_u64.sts': 'mod slice_case;\n'
                                                      "test 'case' {\n"
                                                      '    heap []u64 s = make.([]u64, 3, 5); expect_eq.(len.(s), 3); '
                                                      'expect_eq.(cap.(s), 5); expect_eq.(s[0], 0); expect_eq.(s[2], 0); s[1] = '
                                                      '1; expect_eq.(s[1], 1); rem.(s);\n'
                                                      '}\n',
 'sources/primitive-slices/native/primitive_u8.sts': 'mod slice_case;\n'
                                                     "test 'case' {\n"
                                                     '    heap []u8 s = make.([]u8, 3, 5); expect_eq.(len.(s), 3); '
                                                     'expect_eq.(cap.(s), 5); expect_eq.(s[0], 0); expect_eq.(s[2], 0); s[1] = '
                                                     '1; expect_eq.(s[1], 1); rem.(s);\n'
                                                     '}\n',
 'sources/primitive-slices/native/retained_empty.sts': 'mod slice_case;\n'
                                                       "test 'case' {\n"
                                                       '    heap []i32 s = make.([]i32, 0, 3); expect_eq.(len.(s), 0);\n'
                                                       '}\n',
 'sources/primitive-slices/native/retained_i32.sts': 'mod slice_case;\n'
                                                     "test 'case' {\n"
                                                     '    heap []i32 s = make.([]i32, 3, 5); expect_eq.(len.(s), 3);\n'
                                                     '}\n',
 'sources/primitive-slices/native/schedule_defer_break.sts': 'mod slice_schedule;\n'
                                                             'stack i32 trace = 0;\n'
                                                             'fn f(void): void {\n'
                                                             '    inf { heap []i32 s = make.([]i32, 1); defer { rem.(s); } trace '
                                                             '= len.(s); break; }\n'
                                                             '}\n'
                                                             "test 'schedule' { f(); expect_eq.(trace, 1); }\n",
 'sources/primitive-slices/native/schedule_defer_continue.sts': 'mod slice_schedule;\n'
                                                                'stack i32 trace = 0;\n'
                                                                'fn f(void): void {\n'
                                                                '    stack i32 i = 0; while (i < 3) { heap []i32 s = '
                                                                'make.([]i32, 1); defer { rem.(s); } i += 1; trace += len.(s); '
                                                                'continue; }\n'
                                                                '}\n'
                                                                "test 'schedule' { f(); expect_eq.(trace, 3); }\n",
 'sources/primitive-slices/native/schedule_defer_fallthrough.sts': 'mod slice_schedule;\n'
                                                                   'stack i32 trace = 0;\n'
                                                                   'fn f(void): void {\n'
                                                                   '    heap []i32 s = make.([]i32, 1, 2); defer { rem.(s); } '
                                                                   'trace = len.(s);\n'
                                                                   '}\n'
                                                                   "test 'schedule' { f(); expect_eq.(trace, 1); }\n",
 'sources/primitive-slices/native/schedule_defer_nested.sts': 'mod slice_schedule;\n'
                                                              'stack i32 trace = 0;\n'
                                                              'fn f(void): void {\n'
                                                              '    heap []i32 s = make.([]i32, 1, 2); defer { rem.(s); } { heap '
                                                              '[]i16 t = make.([]i16, 1, 3); defer { rem.(t); } trace = cap.(s) '
                                                              '+ cap.(t); }\n'
                                                              '}\n'
                                                              "test 'schedule' { f(); expect_eq.(trace, 5); }\n",
 'sources/primitive-slices/native/schedule_defer_nested_exits.sts': 'mod slice_schedule;\n'
                                                                    'stack i32 trace = 0;\n'
                                                                    'fn f(void): void {\n'
                                                                    '    heap []u8 outer = make.([]u8, 1, 2); defer { '
                                                                    'rem.(outer); } while (trace < 2) { heap []i16 middle = '
                                                                    'make.([]i16, 1, 3); defer { rem.(middle); } { heap []i32 '
                                                                    'inner = make.([]i32, 1, 4); defer { rem.(inner); } trace += '
                                                                    '1; continue; } }\n'
                                                                    '}\n'
                                                                    "test 'schedule' { f(); expect_eq.(trace, 2); }\n",
 'sources/primitive-slices/native/schedule_defer_return.sts': 'mod slice_schedule;\n'
                                                              'stack i32 trace = 0;\n'
                                                              'fn f(void): void {\n'
                                                              '    heap []i32 s = make.([]i32, 1, 2); defer { rem.(s); } trace = '
                                                              'len.(s); ret;\n'
                                                              '}\n'
                                                              "test 'schedule' { f(); expect_eq.(trace, 1); }\n",
 'sources/primitive-slices/native/schedule_direct.sts': 'mod slice_schedule;\n'
                                                        'stack i32 trace = 0;\n'
                                                        'fn f(void): void {\n'
                                                        '    heap []i32 s = make.([]i32, 1, 2); trace = cap.(s) - 1; rem.(s);\n'
                                                        '}\n'
                                                        "test 'schedule' { f(); expect_eq.(trace, 1); }\n",
 'sources/primitive-slices/native/schedule_direct_bypass_break.sts': 'mod slice_schedule;\n'
                                                                     'stack i32 trace = 0;\n'
                                                                     'fn f(void): void {\n'
                                                                     '    inf { heap []i32 s = make.([]i32, 3, 5); trace = 1; '
                                                                     'break; rem.(s); }\n'
                                                                     '}\n'
                                                                     "test 'schedule' { f(); expect_eq.(trace, 1); }\n",
 'sources/primitive-slices/native/schedule_direct_bypass_continue.sts': 'mod slice_schedule;\n'
                                                                        'stack i32 trace = 0;\n'
                                                                        'fn f(void): void {\n'
                                                                        '    while (trace < 2) { heap []i32 s = make.([]i32, 3, '
                                                                        '5); trace += 1; continue; rem.(s); }\n'
                                                                        '}\n'
                                                                        "test 'schedule' { f(); expect_eq.(trace, 2); }\n",
 'sources/primitive-slices/native/schedule_direct_bypass_return.sts': 'mod slice_schedule;\n'
                                                                      'stack i32 trace = 0;\n'
                                                                      'fn f(void): void {\n'
                                                                      '    heap []i32 s = make.([]i32, 3, 5); trace = 1; ret; '
                                                                      'rem.(s);\n'
                                                                      '}\n'
                                                                      "test 'schedule' { f(); expect_eq.(trace, 1); }\n",
 'sources/primitive-slices/native/schedule_shadow_defer.sts': 'mod slice_schedule;\n'
                                                              'stack i32 trace = 0;\n'
                                                              'fn f(void): void {\n'
                                                              '    heap []i32 s = make.([]i32, 2, 5); defer { rem.(s); } { heap '
                                                              '[]i16 s = make.([]i16, len.(s), 3); defer { rem.(s); } trace = '
                                                              'cap.(s); }\n'
                                                              '}\n'
                                                              "test 'schedule' { f(); expect_eq.(trace, 3); }\n",
 'sources/primitive-slices/native/schedule_shadow_siblings.sts': 'mod slice_schedule;\n'
                                                                 'stack i32 trace = 0;\n'
                                                                 'fn f(void): void {\n'
                                                                 '    { heap []i32 s = make.([]i32, 1, 2); defer { rem.(s); } '
                                                                 'trace += cap.(s); } { heap []i16 s = make.([]i16, 1, 3); defer '
                                                                 '{ rem.(s); } trace += cap.(s); }\n'
                                                                 '}\n'
                                                                 "test 'schedule' { f(); expect_eq.(trace, 5); }\n",
 'sources/primitive-slices/native/zero_explicit.sts': 'mod slice_case;\n'
                                                      "test 'case' {\n"
                                                      '    heap []i32 s = make.([]i32, 0, 0); expect_eq.(len.(s), 0); '
                                                      'expect_eq.(cap.(s), 0); rem.(s);\n'
                                                      '}\n',
 'sources/primitive-slices/native/zero_omitted.sts': 'mod slice_case;\n'
                                                     "test 'case' {\n"
                                                     '    heap []i32 s = make.([]i32, 0); expect_eq.(len.(s), 0); '
                                                     'expect_eq.(cap.(s), 0); rem.(s);\n'
                                                     '}\n',
 'sources/primitive-slices/reject/append-called.sts': 'mod slice_reject;\n'
                                                      'fn bad(void): void {\n'
                                                      '    heap []i32 s = make.([]i32, 1, 2); append.(s, 3);\n'
                                                      '}\n'
                                                      "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/append-dead.sts': 'mod slice_reject;\n'
                                                    'fn bad(void): void {\n'
                                                    '    ret; heap []i32 s = make.([]i32, 1, 2); append.(s, 3);\n'
                                                    '}\n'
                                                    "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/append-uncalled.sts': 'mod slice_reject;\n'
                                                        'fn bad(void): void {\n'
                                                        '    heap []i32 s = make.([]i32, 1, 2); append.(s, 3);\n'
                                                        '}\n'
                                                        "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/assignment_descriptor_query-called.sts': 'mod slice_reject;\n'
                                                                           'fn bad(void): void {\n'
                                                                           '    heap []i32 s = make.([]i32, 1, 2); len.(s = s);\n'
                                                                           '}\n'
                                                                           "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/assignment_descriptor_query-dead.sts': 'mod slice_reject;\n'
                                                                         'fn bad(void): void {\n'
                                                                         '    ret; heap []i32 s = make.([]i32, 1, 2); len.(s = '
                                                                         's);\n'
                                                                         '}\n'
                                                                         "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/assignment_descriptor_query-uncalled.sts': 'mod slice_reject;\n'
                                                                             'fn bad(void): void {\n'
                                                                             '    heap []i32 s = make.([]i32, 1, 2); len.(s = '
                                                                             's);\n'
                                                                             '}\n'
                                                                             "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/captured_use-called.sts': 'mod slice_reject;\n'
                                                            'fn bad(void): void {\n'
                                                            '    heap []i32 s = make.([]i32, 1, 2); stack fn*(void): i32 f = '
                                                            'lam.(void): i32 { ret len.(s); };\n'
                                                            '}\n'
                                                            "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/captured_use-dead.sts': 'mod slice_reject;\n'
                                                          'fn bad(void): void {\n'
                                                          '    ret; heap []i32 s = make.([]i32, 1, 2); stack fn*(void): i32 f = '
                                                          'lam.(void): i32 { ret len.(s); };\n'
                                                          '}\n'
                                                          "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/captured_use-uncalled.sts': 'mod slice_reject;\n'
                                                              'fn bad(void): void {\n'
                                                              '    heap []i32 s = make.([]i32, 1, 2); stack fn*(void): i32 f = '
                                                              'lam.(void): i32 { ret len.(s); };\n'
                                                              '}\n'
                                                              "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/composite_element-called.sts': 'mod slice_reject;\n'
                                                                 'type Box: struct { ext i32 x; };\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    heap []Box s = make.([]Box, 1);\n'
                                                                 '}\n'
                                                                 "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/composite_element-dead.sts': 'mod slice_reject;\n'
                                                               'type Box: struct { ext i32 x; };\n'
                                                               'fn bad(void): void {\n'
                                                               '    ret; heap []Box s = make.([]Box, 1);\n'
                                                               '}\n'
                                                               "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/composite_element-uncalled.sts': 'mod slice_reject;\n'
                                                                   'type Box: struct { ext i32 x; };\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    heap []Box s = make.([]Box, 1);\n'
                                                                   '}\n'
                                                                   "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/compound_element-called.sts': 'mod slice_reject;\n'
                                                                'fn bad(void): void {\n'
                                                                '    heap []i32 s = make.([]i32, 1, 2); s[0] += 1;\n'
                                                                '}\n'
                                                                "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/compound_element-dead.sts': 'mod slice_reject;\n'
                                                              'fn bad(void): void {\n'
                                                              '    ret; heap []i32 s = make.([]i32, 1, 2); s[0] += 1;\n'
                                                              '}\n'
                                                              "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/compound_element-uncalled.sts': 'mod slice_reject;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    heap []i32 s = make.([]i32, 1, 2); s[0] += 1;\n'
                                                                  '}\n'
                                                                  "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/conditional_descriptor_query-called.sts': 'mod slice_reject;\n'
                                                                            'fn bad(void): void {\n'
                                                                            '    heap []i32 s = make.([]i32, 1, 2); len.(true ? '
                                                                            's : s);\n'
                                                                            '}\n'
                                                                            "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/conditional_descriptor_query-dead.sts': 'mod slice_reject;\n'
                                                                          'fn bad(void): void {\n'
                                                                          '    ret; heap []i32 s = make.([]i32, 1, 2); len.(true '
                                                                          '? s : s);\n'
                                                                          '}\n'
                                                                          "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/conditional_descriptor_query-uncalled.sts': 'mod slice_reject;\n'
                                                                              'fn bad(void): void {\n'
                                                                              '    heap []i32 s = make.([]i32, 1, 2); len.(true '
                                                                              '? s : s);\n'
                                                                              '}\n'
                                                                              "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/conditional_release-called.sts': 'mod slice_reject;\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    heap []i32 s = make.([]i32, 1, 2); if (true) { rem.(s); '
                                                                   '}\n'
                                                                   '}\n'
                                                                   "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/conditional_release-dead.sts': 'mod slice_reject;\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    ret; heap []i32 s = make.([]i32, 1, 2); if (true) { '
                                                                 'rem.(s); }\n'
                                                                 '}\n'
                                                                 "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/conditional_release-uncalled.sts': 'mod slice_reject;\n'
                                                                     'fn bad(void): void {\n'
                                                                     '    heap []i32 s = make.([]i32, 1, 2); if (true) { '
                                                                     'rem.(s); }\n'
                                                                     '}\n'
                                                                     "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/copy-called.sts': 'mod slice_reject;\n'
                                                    'fn bad(void): void {\n'
                                                    '    heap []i32 s = make.([]i32, 1, 2); copy.(s, s);\n'
                                                    '}\n'
                                                    "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/copy-dead.sts': 'mod slice_reject;\n'
                                                  'fn bad(void): void {\n'
                                                  '    ret; heap []i32 s = make.([]i32, 1, 2); copy.(s, s);\n'
                                                  '}\n'
                                                  "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/copy-uncalled.sts': 'mod slice_reject;\n'
                                                      'fn bad(void): void {\n'
                                                      '    heap []i32 s = make.([]i32, 1, 2); copy.(s, s);\n'
                                                      '}\n'
                                                      "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/defer_extra_statement-called.sts': 'mod slice_reject;\n'
                                                                     'fn bad(void): void {\n'
                                                                     '    heap []i32 s = make.([]i32, 1, 2); defer { rem.(s); '
                                                                     'expect.(true); }\n'
                                                                     '}\n'
                                                                     "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/defer_extra_statement-dead.sts': 'mod slice_reject;\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    ret; heap []i32 s = make.([]i32, 1, 2); defer { rem.(s); '
                                                                   'expect.(true); }\n'
                                                                   '}\n'
                                                                   "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/defer_extra_statement-uncalled.sts': 'mod slice_reject;\n'
                                                                       'fn bad(void): void {\n'
                                                                       '    heap []i32 s = make.([]i32, 1, 2); defer { rem.(s); '
                                                                       'expect.(true); }\n'
                                                                       '}\n'
                                                                       "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/defer_single_statement-called.sts': 'mod slice_reject;\n'
                                                                      'fn bad(void): void {\n'
                                                                      '    heap []i32 s = make.([]i32, 1, 2); defer rem.(s);\n'
                                                                      '}\n'
                                                                      "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/defer_single_statement-dead.sts': 'mod slice_reject;\n'
                                                                    'fn bad(void): void {\n'
                                                                    '    ret; heap []i32 s = make.([]i32, 1, 2); defer rem.(s);\n'
                                                                    '}\n'
                                                                    "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/defer_single_statement-uncalled.sts': 'mod slice_reject;\n'
                                                                        'fn bad(void): void {\n'
                                                                        '    heap []i32 s = make.([]i32, 1, 2); defer rem.(s);\n'
                                                                        '}\n'
                                                                        "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/deferred_constructor-called.sts': 'mod slice_reject;\n'
                                                                    'fn bad(void): void {\n'
                                                                    '    defer { heap []i32 s = make.([]i32, 1); }\n'
                                                                    '}\n'
                                                                    "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/deferred_constructor-dead.sts': 'mod slice_reject;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    ret; defer { heap []i32 s = make.([]i32, 1); }\n'
                                                                  '}\n'
                                                                  "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/deferred_constructor-uncalled.sts': 'mod slice_reject;\n'
                                                                      'fn bad(void): void {\n'
                                                                      '    defer { heap []i32 s = make.([]i32, 1); }\n'
                                                                      '}\n'
                                                                      "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/deferred_read-called.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    heap []i32 s = make.([]i32, 1, 2); defer { expect_eq.(len.(s), '
                                                             '1); }\n'
                                                             '}\n'
                                                             "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/deferred_read-dead.sts': 'mod slice_reject;\n'
                                                           'fn bad(void): void {\n'
                                                           '    ret; heap []i32 s = make.([]i32, 1, 2); defer { '
                                                           'expect_eq.(len.(s), 1); }\n'
                                                           '}\n'
                                                           "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/deferred_read-uncalled.sts': 'mod slice_reject;\n'
                                                               'fn bad(void): void {\n'
                                                               '    heap []i32 s = make.([]i32, 1, 2); defer { '
                                                               'expect_eq.(len.(s), 1); }\n'
                                                               '}\n'
                                                               "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/delayed_defer-called.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    heap []i32 s = make.([]i32, 1, 2); stack i32 x = 0; defer { '
                                                             'rem.(s); }\n'
                                                             '}\n'
                                                             "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/delayed_defer-dead.sts': 'mod slice_reject;\n'
                                                           'fn bad(void): void {\n'
                                                           '    ret; heap []i32 s = make.([]i32, 1, 2); stack i32 x = 0; defer { '
                                                           'rem.(s); }\n'
                                                           '}\n'
                                                           "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/delayed_defer-uncalled.sts': 'mod slice_reject;\n'
                                                               'fn bad(void): void {\n'
                                                               '    heap []i32 s = make.([]i32, 1, 2); stack i32 x = 0; defer { '
                                                               'rem.(s); }\n'
                                                               '}\n'
                                                               "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/descriptor_address-called.sts': 'mod slice_reject;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    heap []i32 s = make.([]i32, 1, 2); &s;\n'
                                                                  '}\n'
                                                                  "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/descriptor_address-dead.sts': 'mod slice_reject;\n'
                                                                'fn bad(void): void {\n'
                                                                '    ret; heap []i32 s = make.([]i32, 1, 2); &s;\n'
                                                                '}\n'
                                                                "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/descriptor_address-uncalled.sts': 'mod slice_reject;\n'
                                                                    'fn bad(void): void {\n'
                                                                    '    heap []i32 s = make.([]i32, 1, 2); &s;\n'
                                                                    '}\n'
                                                                    "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/descriptor_argument-called.sts': 'mod slice_reject;\n'
                                                                   'fn take(heap []i32 x): void { }\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    heap []i32 s = make.([]i32, 1, 2); take(s);\n'
                                                                   '}\n'
                                                                   "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/descriptor_argument-dead.sts': 'mod slice_reject;\n'
                                                                 'fn take(heap []i32 x): void { }\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    ret; heap []i32 s = make.([]i32, 1, 2); take(s);\n'
                                                                 '}\n'
                                                                 "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/descriptor_argument-uncalled.sts': 'mod slice_reject;\n'
                                                                     'fn take(heap []i32 x): void { }\n'
                                                                     'fn bad(void): void {\n'
                                                                     '    heap []i32 s = make.([]i32, 1, 2); take(s);\n'
                                                                     '}\n'
                                                                     "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/descriptor_assignment-called.sts': 'mod slice_reject;\n'
                                                                     'fn bad(void): void {\n'
                                                                     '    heap []i32 s = make.([]i32, 1, 2); s = make.([]i32, '
                                                                     '1);\n'
                                                                     '}\n'
                                                                     "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/descriptor_assignment-dead.sts': 'mod slice_reject;\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    ret; heap []i32 s = make.([]i32, 1, 2); s = make.([]i32, '
                                                                   '1);\n'
                                                                   '}\n'
                                                                   "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/descriptor_assignment-uncalled.sts': 'mod slice_reject;\n'
                                                                       'fn bad(void): void {\n'
                                                                       '    heap []i32 s = make.([]i32, 1, 2); s = make.([]i32, '
                                                                       '1);\n'
                                                                       '}\n'
                                                                       "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/descriptor_cast-called.sts': 'mod slice_reject;\n'
                                                               'fn bad(void): void {\n'
                                                               '    heap []i32 s = make.([]i32, 1, 2); (i32) s;\n'
                                                               '}\n'
                                                               "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/descriptor_cast-dead.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    ret; heap []i32 s = make.([]i32, 1, 2); (i32) s;\n'
                                                             '}\n'
                                                             "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/descriptor_cast-uncalled.sts': 'mod slice_reject;\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    heap []i32 s = make.([]i32, 1, 2); (i32) s;\n'
                                                                 '}\n'
                                                                 "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/descriptor_copy-called.sts': 'mod slice_reject;\n'
                                                               'fn bad(void): void {\n'
                                                               '    heap []i32 s = make.([]i32, 1, 2); heap []i32 t = s;\n'
                                                               '}\n'
                                                               "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/descriptor_copy-dead.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    ret; heap []i32 s = make.([]i32, 1, 2); heap []i32 t = s;\n'
                                                             '}\n'
                                                             "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/descriptor_copy-uncalled.sts': 'mod slice_reject;\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    heap []i32 s = make.([]i32, 1, 2); heap []i32 t = s;\n'
                                                                 '}\n'
                                                                 "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/descriptor_result-called.sts': 'mod slice_reject;\n'
                                                                 'type View: []i32; fn give(void): View { heap []i32 x = '
                                                                 'make.([]i32, 1); ret x; }\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    heap []i32 s = make.([]i32, 1, 2); give();\n'
                                                                 '}\n'
                                                                 "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/descriptor_result-dead.sts': 'mod slice_reject;\n'
                                                               'type View: []i32; fn give(void): View { heap []i32 x = '
                                                               'make.([]i32, 1); ret x; }\n'
                                                               'fn bad(void): void {\n'
                                                               '    ret; heap []i32 s = make.([]i32, 1, 2); give();\n'
                                                               '}\n'
                                                               "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/descriptor_result-uncalled.sts': 'mod slice_reject;\n'
                                                                   'type View: []i32; fn give(void): View { heap []i32 x = '
                                                                   'make.([]i32, 1); ret x; }\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    heap []i32 s = make.([]i32, 1, 2); give();\n'
                                                                   '}\n'
                                                                   "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/direct_with_deferred_read-called.sts': 'mod slice_reject;\n'
                                                                         'fn bad(void): void {\n'
                                                                         '    heap []i32 s = make.([]i32, 1, 2); defer { '
                                                                         'expect_eq.(s[0], 0); } rem.(s);\n'
                                                                         '}\n'
                                                                         "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/direct_with_deferred_read-dead.sts': 'mod slice_reject;\n'
                                                                       'fn bad(void): void {\n'
                                                                       '    ret; heap []i32 s = make.([]i32, 1, 2); defer { '
                                                                       'expect_eq.(s[0], 0); } rem.(s);\n'
                                                                       '}\n'
                                                                       "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/direct_with_deferred_read-uncalled.sts': 'mod slice_reject;\n'
                                                                           'fn bad(void): void {\n'
                                                                           '    heap []i32 s = make.([]i32, 1, 2); defer { '
                                                                           'expect_eq.(s[0], 0); } rem.(s);\n'
                                                                           '}\n'
                                                                           "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/discarded_declaration-called.sts': 'mod slice_reject;\n'
                                                                     'fn bad(void): void {\n'
                                                                     '    heap []i32 _ = make.([]i32, 1);\n'
                                                                     '}\n'
                                                                     "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/discarded_declaration-dead.sts': 'mod slice_reject;\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    ret; heap []i32 _ = make.([]i32, 1);\n'
                                                                   '}\n'
                                                                   "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/discarded_declaration-uncalled.sts': 'mod slice_reject;\n'
                                                                       'fn bad(void): void {\n'
                                                                       '    heap []i32 _ = make.([]i32, 1);\n'
                                                                       '}\n'
                                                                       "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/double_release-called.sts': 'mod slice_reject;\n'
                                                              'fn bad(void): void {\n'
                                                              '    heap []i32 s = make.([]i32, 1, 2); rem.(s); rem.(s);\n'
                                                              '}\n'
                                                              "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/double_release-dead.sts': 'mod slice_reject;\n'
                                                            'fn bad(void): void {\n'
                                                            '    ret; heap []i32 s = make.([]i32, 1, 2); rem.(s); rem.(s);\n'
                                                            '}\n'
                                                            "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/double_release-uncalled.sts': 'mod slice_reject;\n'
                                                                'fn bad(void): void {\n'
                                                                '    heap []i32 s = make.([]i32, 1, 2); rem.(s); rem.(s);\n'
                                                                '}\n'
                                                                "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/element_address-called.sts': 'mod slice_reject;\n'
                                                               'fn bad(void): void {\n'
                                                               '    heap []i32 s = make.([]i32, 1, 2); &s[0];\n'
                                                               '}\n'
                                                               "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/element_address-dead.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    ret; heap []i32 s = make.([]i32, 1, 2); &s[0];\n'
                                                             '}\n'
                                                             "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/element_address-uncalled.sts': 'mod slice_reject;\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    heap []i32 s = make.([]i32, 1, 2); &s[0];\n'
                                                                 '}\n'
                                                                 "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/element_alias-called.sts': 'mod slice_reject;\n'
                                                             'type Alias: i32;\n'
                                                             'fn bad(void): void {\n'
                                                             '    heap []Alias s = make.([]Alias, 1);\n'
                                                             '}\n'
                                                             "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/element_alias-dead.sts': 'mod slice_reject;\n'
                                                           'type Alias: i32;\n'
                                                           'fn bad(void): void {\n'
                                                           '    ret; heap []Alias s = make.([]Alias, 1);\n'
                                                           '}\n'
                                                           "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/element_alias-uncalled.sts': 'mod slice_reject;\n'
                                                               'type Alias: i32;\n'
                                                               'fn bad(void): void {\n'
                                                               '    heap []Alias s = make.([]Alias, 1);\n'
                                                               '}\n'
                                                               "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/fat_arrow_constructor-called.sts': 'mod slice_reject;\n'
                                                                     'fn bad(void): void {\n'
                                                                     '    if (true) => heap []i32 s = make.([]i32, 1);\n'
                                                                     '}\n'
                                                                     "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/fat_arrow_constructor-dead.sts': 'mod slice_reject;\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    ret; if (true) => heap []i32 s = make.([]i32, 1);\n'
                                                                   '}\n'
                                                                   "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/fat_arrow_constructor-uncalled.sts': 'mod slice_reject;\n'
                                                                       'fn bad(void): void {\n'
                                                                       '    if (true) => heap []i32 s = make.([]i32, 1);\n'
                                                                       '}\n'
                                                                       "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/float_count-called.sts': 'mod slice_reject;\n'
                                                           'fn bad(void): void {\n'
                                                           '    heap []i32 s = make.([]i32, 1.0);\n'
                                                           '}\n'
                                                           "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/float_count-dead.sts': 'mod slice_reject;\n'
                                                         'fn bad(void): void {\n'
                                                         '    ret; heap []i32 s = make.([]i32, 1.0);\n'
                                                         '}\n'
                                                         "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/float_count-uncalled.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    heap []i32 s = make.([]i32, 1.0);\n'
                                                             '}\n'
                                                             "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/for_initializer-called.sts': 'mod slice_reject;\n'
                                                               'fn bad(void): void {\n'
                                                               '    for (heap []i32 s = make.([]i32, 1); false; 0) { }\n'
                                                               '}\n'
                                                               "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/for_initializer-dead.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    ret; for (heap []i32 s = make.([]i32, 1); false; 0) { }\n'
                                                             '}\n'
                                                             "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/for_initializer-uncalled.sts': 'mod slice_reject;\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    for (heap []i32 s = make.([]i32, 1); false; 0) { }\n'
                                                                 '}\n'
                                                                 "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/foreach-called.sts': 'mod slice_reject;\n'
                                                       'fn bad(void): void {\n'
                                                       '    heap []i32 s = make.([]i32, 1, 2); foreach x in s { expect_eq.(x, '
                                                       '0); }\n'
                                                       '}\n'
                                                       "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/foreach-dead.sts': 'mod slice_reject;\n'
                                                     'fn bad(void): void {\n'
                                                     '    ret; heap []i32 s = make.([]i32, 1, 2); foreach x in s { expect_eq.(x, '
                                                     '0); }\n'
                                                     '}\n'
                                                     "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/foreach-uncalled.sts': 'mod slice_reject;\n'
                                                         'fn bad(void): void {\n'
                                                         '    heap []i32 s = make.([]i32, 1, 2); foreach x in s { expect_eq.(x, '
                                                         '0); }\n'
                                                         '}\n'
                                                         "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/generic_constructor-called.sts': 'mod slice_reject;\n'
                                                                   'type GenericView: []T; fn @comptime[T] construct(stack i32 '
                                                                   'n): GenericView { ret make.([]T, n); }\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    heap []i32 s = construct.[i32](1);\n'
                                                                   '}\n'
                                                                   "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/generic_constructor-dead.sts': 'mod slice_reject;\n'
                                                                 'type GenericView: []T; fn @comptime[T] construct(stack i32 n): '
                                                                 'GenericView { ret make.([]T, n); }\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    ret; heap []i32 s = construct.[i32](1);\n'
                                                                 '}\n'
                                                                 "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/generic_constructor-uncalled.sts': 'mod slice_reject;\n'
                                                                     'type GenericView: []T; fn @comptime[T] construct(stack i32 '
                                                                     'n): GenericView { ret make.([]T, n); }\n'
                                                                     'fn bad(void): void {\n'
                                                                     '    heap []i32 s = construct.[i32](1);\n'
                                                                     '}\n'
                                                                     "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/global_slice.sts': 'mod slice_reject;\n'
                                                     'heap []i32 s = make.([]i32, 1);\n'
                                                     "test 'must not launch' { }\n",
 'sources/primitive-slices/reject/inferred_declaration-called.sts': 'mod slice_reject;\n'
                                                                    'fn bad(void): void {\n'
                                                                    '    heap let s = make.([]i32, 1);\n'
                                                                    '}\n'
                                                                    "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/inferred_declaration-dead.sts': 'mod slice_reject;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    ret; heap let s = make.([]i32, 1);\n'
                                                                  '}\n'
                                                                  "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/inferred_declaration-uncalled.sts': 'mod slice_reject;\n'
                                                                      'fn bad(void): void {\n'
                                                                      '    heap let s = make.([]i32, 1);\n'
                                                                      '}\n'
                                                                      "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/inline_make-called.sts': 'mod slice_reject;\n'
                                                           'fn bad(void): void {\n'
                                                           '    heap []i32 s = make.{1};\n'
                                                           '}\n'
                                                           "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/inline_make-dead.sts': 'mod slice_reject;\n'
                                                         'fn bad(void): void {\n'
                                                         '    ret; heap []i32 s = make.{1};\n'
                                                         '}\n'
                                                         "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/inline_make-uncalled.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    heap []i32 s = make.{1};\n'
                                                             '}\n'
                                                             "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/loop_release_outer-called.sts': 'mod slice_reject;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    heap []i32 s = make.([]i32, 1, 2); inf { rem.(s); break; '
                                                                  '}\n'
                                                                  '}\n'
                                                                  "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/loop_release_outer-dead.sts': 'mod slice_reject;\n'
                                                                'fn bad(void): void {\n'
                                                                '    ret; heap []i32 s = make.([]i32, 1, 2); inf { rem.(s); '
                                                                'break; }\n'
                                                                '}\n'
                                                                "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/loop_release_outer-uncalled.sts': 'mod slice_reject;\n'
                                                                    'fn bad(void): void {\n'
                                                                    '    heap []i32 s = make.([]i32, 1, 2); inf { rem.(s); '
                                                                    'break; }\n'
                                                                    '}\n'
                                                                    "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/member_query-called.sts': 'mod slice_reject;\n'
                                                            'fn bad(void): void {\n'
                                                            '    heap []i32 s = make.([]i32, 1, 2); len.(s.len);\n'
                                                            '}\n'
                                                            "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/member_query-dead.sts': 'mod slice_reject;\n'
                                                          'fn bad(void): void {\n'
                                                          '    ret; heap []i32 s = make.([]i32, 1, 2); len.(s.len);\n'
                                                          '}\n'
                                                          "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/member_query-uncalled.sts': 'mod slice_reject;\n'
                                                              'fn bad(void): void {\n'
                                                              '    heap []i32 s = make.([]i32, 1, 2); len.(s.len);\n'
                                                              '}\n'
                                                              "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/nested_constructor-called.sts': 'mod slice_reject;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    heap []i32 s = make.([]i32, len.(make.([]i32, 1)));\n'
                                                                  '}\n'
                                                                  "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/nested_constructor-dead.sts': 'mod slice_reject;\n'
                                                                'fn bad(void): void {\n'
                                                                '    ret; heap []i32 s = make.([]i32, len.(make.([]i32, 1)));\n'
                                                                '}\n'
                                                                "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/nested_constructor-uncalled.sts': 'mod slice_reject;\n'
                                                                    'fn bad(void): void {\n'
                                                                    '    heap []i32 s = make.([]i32, len.(make.([]i32, 1)));\n'
                                                                    '}\n'
                                                                    "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/nested_element_address-called.sts': 'mod slice_reject;\n'
                                                                      'fn take_ptr(stack i32 *r p): void { }\n'
                                                                      'fn bad(void): void {\n'
                                                                      '    heap []i32 s = make.([]i32, 1, 2); take_ptr(&s[0]);\n'
                                                                      '}\n'
                                                                      "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/nested_element_address-dead.sts': 'mod slice_reject;\n'
                                                                    'fn take_ptr(stack i32 *r p): void { }\n'
                                                                    'fn bad(void): void {\n'
                                                                    '    ret; heap []i32 s = make.([]i32, 1, 2); '
                                                                    'take_ptr(&s[0]);\n'
                                                                    '}\n'
                                                                    "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/nested_element_address-uncalled.sts': 'mod slice_reject;\n'
                                                                        'fn take_ptr(stack i32 *r p): void { }\n'
                                                                        'fn bad(void): void {\n'
                                                                        '    heap []i32 s = make.([]i32, 1, 2); '
                                                                        'take_ptr(&s[0]);\n'
                                                                        '}\n'
                                                                        "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/nested_element_type-called.sts': 'mod slice_reject;\n'
                                                                   'type View: []i32;\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    heap []View s = make.([]View, 1);\n'
                                                                   '}\n'
                                                                   "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/nested_element_type-dead.sts': 'mod slice_reject;\n'
                                                                 'type View: []i32;\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    ret; heap []View s = make.([]View, 1);\n'
                                                                 '}\n'
                                                                 "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/nested_element_type-uncalled.sts': 'mod slice_reject;\n'
                                                                     'type View: []i32;\n'
                                                                     'fn bad(void): void {\n'
                                                                     '    heap []View s = make.([]View, 1);\n'
                                                                     '}\n'
                                                                     "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/nested_release-called.sts': 'mod slice_reject;\n'
                                                              'fn bad(void): void {\n'
                                                              '    heap []i32 s = make.([]i32, 1, 2); { rem.(s); }\n'
                                                              '}\n'
                                                              "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/nested_release-dead.sts': 'mod slice_reject;\n'
                                                            'fn bad(void): void {\n'
                                                            '    ret; heap []i32 s = make.([]i32, 1, 2); { rem.(s); }\n'
                                                            '}\n'
                                                            "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/nested_release-uncalled.sts': 'mod slice_reject;\n'
                                                                'fn bad(void): void {\n'
                                                                '    heap []i32 s = make.([]i32, 1, 2); { rem.(s); }\n'
                                                                '}\n'
                                                                "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/nil_declaration-called.sts': 'mod slice_reject;\n'
                                                               'fn bad(void): void {\n'
                                                               '    heap []i32 s = nil;\n'
                                                               '}\n'
                                                               "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/nil_declaration-dead.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    ret; heap []i32 s = nil;\n'
                                                             '}\n'
                                                             "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/nil_declaration-uncalled.sts': 'mod slice_reject;\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    heap []i32 s = nil;\n'
                                                                 '}\n'
                                                                 "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/pointer_count-called.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    stack i32 x = 1; heap []i32 s = make.([]i32, &x);\n'
                                                             '}\n'
                                                             "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/pointer_count-dead.sts': 'mod slice_reject;\n'
                                                           'fn bad(void): void {\n'
                                                           '    ret; stack i32 x = 1; heap []i32 s = make.([]i32, &x);\n'
                                                           '}\n'
                                                           "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/pointer_count-uncalled.sts': 'mod slice_reject;\n'
                                                               'fn bad(void): void {\n'
                                                               '    stack i32 x = 1; heap []i32 s = make.([]i32, &x);\n'
                                                               '}\n'
                                                               "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/pointer_element_type-called.sts': 'mod slice_reject;\n'
                                                                    'type P: i32 *rw;\n'
                                                                    'fn bad(void): void {\n'
                                                                    '    heap []P s = make.([]P, 1);\n'
                                                                    '}\n'
                                                                    "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/pointer_element_type-dead.sts': 'mod slice_reject;\n'
                                                                  'type P: i32 *rw;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    ret; heap []P s = make.([]P, 1);\n'
                                                                  '}\n'
                                                                  "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/pointer_element_type-uncalled.sts': 'mod slice_reject;\n'
                                                                      'type P: i32 *rw;\n'
                                                                      'fn bad(void): void {\n'
                                                                      '    heap []P s = make.([]P, 1);\n'
                                                                      '}\n'
                                                                      "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/postfix_element-called.sts': 'mod slice_reject;\n'
                                                               'fn bad(void): void {\n'
                                                               '    heap []i32 s = make.([]i32, 1, 2); s[0]++;\n'
                                                               '}\n'
                                                               "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/postfix_element-dead.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    ret; heap []i32 s = make.([]i32, 1, 2); s[0]++;\n'
                                                             '}\n'
                                                             "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/postfix_element-uncalled.sts': 'mod slice_reject;\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    heap []i32 s = make.([]i32, 1, 2); s[0]++;\n'
                                                                 '}\n'
                                                                 "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/qualified_declaration-called.sts': 'mod slice_reject;\n'
                                                                     'fn bad(void): void {\n'
                                                                     '    heap const []i32 s = make.([]i32, 1);\n'
                                                                     '}\n'
                                                                     "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/qualified_declaration-dead.sts': 'mod slice_reject;\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    ret; heap const []i32 s = make.([]i32, 1);\n'
                                                                   '}\n'
                                                                   "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/qualified_declaration-uncalled.sts': 'mod slice_reject;\n'
                                                                       'fn bad(void): void {\n'
                                                                       '    heap const []i32 s = make.([]i32, 1);\n'
                                                                       '}\n'
                                                                       "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/query_descriptor_call.sts': 'mod slice_reject;\n'
                                                              'type View: []i32; fn give(void): View { heap []i32 s = '
                                                              'make.([]i32, 1); ret s; }\n'
                                                              "test 'must not launch' { len.(give()); }\n",
 'sources/primitive-slices/reject/query_raw_pointer.sts': 'mod slice_reject;\n'
                                                          "test 'must not launch' { heap i32 *rw p = new.(4); len.(p); rem.(p); "
                                                          '}\n',
 'sources/primitive-slices/reject/release_expression-called.sts': 'mod slice_reject;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    heap []i32 s = make.([]i32, 1, 2); stack i32 x = '
                                                                  'rem.(s);\n'
                                                                  '}\n'
                                                                  "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/release_expression-dead.sts': 'mod slice_reject;\n'
                                                                'fn bad(void): void {\n'
                                                                '    ret; heap []i32 s = make.([]i32, 1, 2); stack i32 x = '
                                                                'rem.(s);\n'
                                                                '}\n'
                                                                "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/release_expression-uncalled.sts': 'mod slice_reject;\n'
                                                                    'fn bad(void): void {\n'
                                                                    '    heap []i32 s = make.([]i32, 1, 2); stack i32 x = '
                                                                    'rem.(s);\n'
                                                                    '}\n'
                                                                    "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/release_plus_defer-called.sts': 'mod slice_reject;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    heap []i32 s = make.([]i32, 1, 2); defer { rem.(s); } '
                                                                  'rem.(s);\n'
                                                                  '}\n'
                                                                  "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/release_plus_defer-dead.sts': 'mod slice_reject;\n'
                                                                'fn bad(void): void {\n'
                                                                '    ret; heap []i32 s = make.([]i32, 1, 2); defer { rem.(s); } '
                                                                'rem.(s);\n'
                                                                '}\n'
                                                                "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/release_plus_defer-uncalled.sts': 'mod slice_reject;\n'
                                                                    'fn bad(void): void {\n'
                                                                    '    heap []i32 s = make.([]i32, 1, 2); defer { rem.(s); } '
                                                                    'rem.(s);\n'
                                                                    '}\n'
                                                                    "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/release_then_nested_use-called.sts': 'mod slice_reject;\n'
                                                                       'fn bad(void): void {\n'
                                                                       '    heap []i32 s = make.([]i32, 1, 2); rem.(s); { '
                                                                       'expect_eq.(s[0], 0); }\n'
                                                                       '}\n'
                                                                       "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/release_then_nested_use-dead.sts': 'mod slice_reject;\n'
                                                                     'fn bad(void): void {\n'
                                                                     '    ret; heap []i32 s = make.([]i32, 1, 2); rem.(s); { '
                                                                     'expect_eq.(s[0], 0); }\n'
                                                                     '}\n'
                                                                     "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/release_then_nested_use-uncalled.sts': 'mod slice_reject;\n'
                                                                         'fn bad(void): void {\n'
                                                                         '    heap []i32 s = make.([]i32, 1, 2); rem.(s); { '
                                                                         'expect_eq.(s[0], 0); }\n'
                                                                         '}\n'
                                                                         "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/release_then_use-called.sts': 'mod slice_reject;\n'
                                                                'fn bad(void): void {\n'
                                                                '    heap []i32 s = make.([]i32, 1, 2); rem.(s); '
                                                                'expect_eq.(len.(s), 1);\n'
                                                                '}\n'
                                                                "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/release_then_use-dead.sts': 'mod slice_reject;\n'
                                                              'fn bad(void): void {\n'
                                                              '    ret; heap []i32 s = make.([]i32, 1, 2); rem.(s); '
                                                              'expect_eq.(len.(s), 1);\n'
                                                              '}\n'
                                                              "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/release_then_use-uncalled.sts': 'mod slice_reject;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    heap []i32 s = make.([]i32, 1, 2); rem.(s); '
                                                                  'expect_eq.(len.(s), 1);\n'
                                                                  '}\n'
                                                                  "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/slice_array-called.sts': 'mod slice_reject;\n'
                                                           'fn bad(void): void {\n'
                                                           '    heap []i32 s = make.([]i32, 1, 2); heap []i32 values[1] = .{s};\n'
                                                           '}\n'
                                                           "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/slice_array-dead.sts': 'mod slice_reject;\n'
                                                         'fn bad(void): void {\n'
                                                         '    ret; heap []i32 s = make.([]i32, 1, 2); heap []i32 values[1] = '
                                                         '.{s};\n'
                                                         '}\n'
                                                         "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/slice_array-uncalled.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    heap []i32 s = make.([]i32, 1, 2); heap []i32 values[1] = '
                                                             '.{s};\n'
                                                             '}\n'
                                                             "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/stack_declaration-called.sts': 'mod slice_reject;\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    stack []i32 s = make.([]i32, 1);\n'
                                                                 '}\n'
                                                                 "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/stack_declaration-dead.sts': 'mod slice_reject;\n'
                                                               'fn bad(void): void {\n'
                                                               '    ret; stack []i32 s = make.([]i32, 1);\n'
                                                               '}\n'
                                                               "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/stack_declaration-uncalled.sts': 'mod slice_reject;\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    stack []i32 s = make.([]i32, 1);\n'
                                                                   '}\n'
                                                                   "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/synthetic_match_binding-called.sts': 'mod slice_reject;\n'
                                                                       'fn bad(void): void {\n'
                                                                       '    heap []i32 s = make.([]i32, 1, 2); match s { x => { '
                                                                       'rem.(x); } }\n'
                                                                       '}\n'
                                                                       "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/synthetic_match_binding-dead.sts': 'mod slice_reject;\n'
                                                                     'fn bad(void): void {\n'
                                                                     '    ret; heap []i32 s = make.([]i32, 1, 2); match s { x => '
                                                                     '{ rem.(x); } }\n'
                                                                     '}\n'
                                                                     "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/synthetic_match_binding-uncalled.sts': 'mod slice_reject;\n'
                                                                         'fn bad(void): void {\n'
                                                                         '    heap []i32 s = make.([]i32, 1, 2); match s { x => '
                                                                         '{ rem.(x); } }\n'
                                                                         '}\n'
                                                                         "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/unchecked_slice_use-called.sts': 'mod slice_reject;\n'
                                                                   'fn bad(void): void {\n'
                                                                   '    heap []i32 s = make.([]i32, 1, 2); '
                                                                   'expect_eq.(s[unchecked: 0], 0);\n'
                                                                   '}\n'
                                                                   "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/unchecked_slice_use-dead.sts': 'mod slice_reject;\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    ret; heap []i32 s = make.([]i32, 1, 2); '
                                                                 'expect_eq.(s[unchecked: 0], 0);\n'
                                                                 '}\n'
                                                                 "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/unchecked_slice_use-uncalled.sts': 'mod slice_reject;\n'
                                                                     'fn bad(void): void {\n'
                                                                     '    heap []i32 s = make.([]i32, 1, 2); '
                                                                     'expect_eq.(s[unchecked: 0], 0);\n'
                                                                     '}\n'
                                                                     "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/uninitialized-called.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    heap []i32 s;\n'
                                                             '}\n'
                                                             "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/uninitialized-dead.sts': 'mod slice_reject;\n'
                                                           'fn bad(void): void {\n'
                                                           '    ret; heap []i32 s;\n'
                                                           '}\n'
                                                           "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/uninitialized-uncalled.sts': 'mod slice_reject;\n'
                                                               'fn bad(void): void {\n'
                                                               '    heap []i32 s;\n'
                                                               '}\n'
                                                               "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/unsafe_slice_use-called.sts': 'mod slice_reject;\n'
                                                                'fn bad(void): void {\n'
                                                                '    heap []i32 s = make.([]i32, 1, 2); unsafe { '
                                                                'expect_eq.(s[0], 0); }\n'
                                                                '}\n'
                                                                "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/unsafe_slice_use-dead.sts': 'mod slice_reject;\n'
                                                              'fn bad(void): void {\n'
                                                              '    ret; heap []i32 s = make.([]i32, 1, 2); unsafe { '
                                                              'expect_eq.(s[0], 0); }\n'
                                                              '}\n'
                                                              "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/unsafe_slice_use-uncalled.sts': 'mod slice_reject;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    heap []i32 s = make.([]i32, 1, 2); unsafe { '
                                                                  'expect_eq.(s[0], 0); }\n'
                                                                  '}\n'
                                                                  "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/view-called.sts': 'mod slice_reject;\n'
                                                    'fn bad(void): void {\n'
                                                    '    heap []i32 s = make.([]i32, 1, 2); heap []i32 t = s[:1];\n'
                                                    '}\n'
                                                    "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/view-dead.sts': 'mod slice_reject;\n'
                                                  'fn bad(void): void {\n'
                                                  '    ret; heap []i32 s = make.([]i32, 1, 2); heap []i32 t = s[:1];\n'
                                                  '}\n'
                                                  "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/view-uncalled.sts': 'mod slice_reject;\n'
                                                      'fn bad(void): void {\n'
                                                      '    heap []i32 s = make.([]i32, 1, 2); heap []i32 t = s[:1];\n'
                                                      '}\n'
                                                      "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/whole_assertion-called.sts': 'mod slice_reject;\n'
                                                               'fn bad(void): void {\n'
                                                               '    heap []i32 s = make.([]i32, 1, 2); expect_eq.(s, s);\n'
                                                               '}\n'
                                                               "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/whole_assertion-dead.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             '    ret; heap []i32 s = make.([]i32, 1, 2); expect_eq.(s, s);\n'
                                                             '}\n'
                                                             "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/whole_assertion-uncalled.sts': 'mod slice_reject;\n'
                                                                 'fn bad(void): void {\n'
                                                                 '    heap []i32 s = make.([]i32, 1, 2); expect_eq.(s, s);\n'
                                                                 '}\n'
                                                                 "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/whole_comparison-called.sts': 'mod slice_reject;\n'
                                                                'fn bad(void): void {\n'
                                                                '    heap []i32 s = make.([]i32, 1, 2); expect.(s == s);\n'
                                                                '}\n'
                                                                "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/whole_comparison-dead.sts': 'mod slice_reject;\n'
                                                              'fn bad(void): void {\n'
                                                              '    ret; heap []i32 s = make.([]i32, 1, 2); expect.(s == s);\n'
                                                              '}\n'
                                                              "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/whole_comparison-uncalled.sts': 'mod slice_reject;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    heap []i32 s = make.([]i32, 1, 2); expect.(s == s);\n'
                                                                  '}\n'
                                                                  "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/whole_expression-called.sts': 'mod slice_reject;\n'
                                                                'fn bad(void): void {\n'
                                                                '    heap []i32 s = make.([]i32, 1, 2); s;\n'
                                                                '}\n'
                                                                "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/whole_expression-dead.sts': 'mod slice_reject;\n'
                                                              'fn bad(void): void {\n'
                                                              '    ret; heap []i32 s = make.([]i32, 1, 2); s;\n'
                                                              '}\n'
                                                              "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/whole_expression-uncalled.sts': 'mod slice_reject;\n'
                                                                  'fn bad(void): void {\n'
                                                                  '    heap []i32 s = make.([]i32, 1, 2); s;\n'
                                                                  '}\n'
                                                                  "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/whole_print-called.sts': 'mod slice_reject;\n'
                                                           'fn bad(void): void {\n'
                                                           "    heap []i32 s = make.([]i32, 1, 2); print.('{}', s);\n"
                                                           '}\n'
                                                           "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/whole_print-dead.sts': 'mod slice_reject;\n'
                                                         'fn bad(void): void {\n'
                                                         "    ret; heap []i32 s = make.([]i32, 1, 2); print.('{}', s);\n"
                                                         '}\n'
                                                         "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/whole_print-uncalled.sts': 'mod slice_reject;\n'
                                                             'fn bad(void): void {\n'
                                                             "    heap []i32 s = make.([]i32, 1, 2); print.('{}', s);\n"
                                                             '}\n'
                                                             "test 'must not launch' {  }\n",
 'sources/primitive-slices/reject/wrong_element_identity-called.sts': 'mod slice_reject;\n'
                                                                      'fn bad(void): void {\n'
                                                                      '    heap []u32 s = make.([]i32, 1);\n'
                                                                      '}\n'
                                                                      "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/wrong_element_identity-dead.sts': 'mod slice_reject;\n'
                                                                    'fn bad(void): void {\n'
                                                                    '    ret; heap []u32 s = make.([]i32, 1);\n'
                                                                    '}\n'
                                                                    "test 'must not launch' { bad(); }\n",
 'sources/primitive-slices/reject/wrong_element_identity-uncalled.sts': 'mod slice_reject;\n'
                                                                        'fn bad(void): void {\n'
                                                                        '    heap []u32 s = make.([]i32, 1);\n'
                                                                        '}\n'
                                                                        "test 'must not launch' {  }\n"}

ORACLES = {'primitive_i8': {'pairs': [],
                  'hits': [],
                  'assertions': 5,
                  'tests': 1,
                  'event_bytes': [5],
                  'allocations': [],
                  'trace': None,
                  'decl_id': 0,
                  'owner': 'case',
                  'static_test_id': 0,
                  'sites': [{'id': 0,
                             'file_id': 0,
                             'line': 3,
                             'col': 19,
                             'end_line': 3,
                             'end_col': 35,
                             'kind': 4,
                             'decl_id': 0,
                             'owner': 'case',
                             'static_test_id': 0,
                             'slice_element_kind': 1,
                             'slice_abi_stride': 1,
                             'slice_count_arity': 2,
                             'binding': 's',
                             'binding_origin_site': 0},
                            {'id': 1,
                             'file_id': 0,
                             'line': 3,
                             'col': 159,
                             'end_line': 3,
                             'end_col': 165,
                             'kind': 5,
                             'decl_id': 0,
                             'owner': 'case',
                             'static_test_id': 0,
                             'slice_element_kind': 0,
                             'slice_abi_stride': 0,
                             'slice_count_arity': 0,
                             'binding': 's',
                             'binding_origin_site': 0}],
                  'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                         'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                               'tracked_outstanding_bytes': 0}},
                  'live_allocations': 0,
                  'tracked_outstanding_bytes': 0,
                  'bitmap': '-',
                  'sources': ['sources/primitive-slices/native/primitive_i8.sts'],
                  'source_hashes': {'sources/primitive-slices/native/primitive_i8.sts': 'c4f0cd1ae0a0820dc06480bcee24eb3d8d8e3d8f8222b0a8418ad3768d7df4ef'}},
 'primitive_u8': {'pairs': [],
                  'hits': [],
                  'assertions': 5,
                  'tests': 1,
                  'event_bytes': [5],
                  'allocations': [],
                  'trace': None,
                  'decl_id': 0,
                  'owner': 'case',
                  'static_test_id': 0,
                  'sites': [{'id': 0,
                             'file_id': 0,
                             'line': 3,
                             'col': 19,
                             'end_line': 3,
                             'end_col': 35,
                             'kind': 4,
                             'decl_id': 0,
                             'owner': 'case',
                             'static_test_id': 0,
                             'slice_element_kind': 2,
                             'slice_abi_stride': 1,
                             'slice_count_arity': 2,
                             'binding': 's',
                             'binding_origin_site': 0},
                            {'id': 1,
                             'file_id': 0,
                             'line': 3,
                             'col': 159,
                             'end_line': 3,
                             'end_col': 165,
                             'kind': 5,
                             'decl_id': 0,
                             'owner': 'case',
                             'static_test_id': 0,
                             'slice_element_kind': 0,
                             'slice_abi_stride': 0,
                             'slice_count_arity': 0,
                             'binding': 's',
                             'binding_origin_site': 0}],
                  'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                         'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                               'tracked_outstanding_bytes': 0}},
                  'live_allocations': 0,
                  'tracked_outstanding_bytes': 0,
                  'bitmap': '-',
                  'sources': ['sources/primitive-slices/native/primitive_u8.sts'],
                  'source_hashes': {'sources/primitive-slices/native/primitive_u8.sts': '2ac3a79e1f975f53b5ba0098457015503534879ff451c1dccf30766dbf722fed'}},
 'primitive_i16': {'pairs': [],
                   'hits': [],
                   'assertions': 5,
                   'tests': 1,
                   'event_bytes': [10],
                   'allocations': [],
                   'trace': None,
                   'decl_id': 0,
                   'owner': 'case',
                   'static_test_id': 0,
                   'sites': [{'id': 0,
                              'file_id': 0,
                              'line': 3,
                              'col': 20,
                              'end_line': 3,
                              'end_col': 37,
                              'kind': 4,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 3,
                              'slice_abi_stride': 2,
                              'slice_count_arity': 2,
                              'binding': 's',
                              'binding_origin_site': 0},
                             {'id': 1,
                              'file_id': 0,
                              'line': 3,
                              'col': 161,
                              'end_line': 3,
                              'end_col': 167,
                              'kind': 5,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 0,
                              'slice_abi_stride': 0,
                              'slice_count_arity': 0,
                              'binding': 's',
                              'binding_origin_site': 0}],
                   'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                          'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                'tracked_outstanding_bytes': 0}},
                   'live_allocations': 0,
                   'tracked_outstanding_bytes': 0,
                   'bitmap': '-',
                   'sources': ['sources/primitive-slices/native/primitive_i16.sts'],
                   'source_hashes': {'sources/primitive-slices/native/primitive_i16.sts': '0b31cf433cf40f5ccdd6887a2db162ad6654a126c8fce86c056229a64e850b30'}},
 'primitive_u16': {'pairs': [],
                   'hits': [],
                   'assertions': 5,
                   'tests': 1,
                   'event_bytes': [10],
                   'allocations': [],
                   'trace': None,
                   'decl_id': 0,
                   'owner': 'case',
                   'static_test_id': 0,
                   'sites': [{'id': 0,
                              'file_id': 0,
                              'line': 3,
                              'col': 20,
                              'end_line': 3,
                              'end_col': 37,
                              'kind': 4,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 4,
                              'slice_abi_stride': 2,
                              'slice_count_arity': 2,
                              'binding': 's',
                              'binding_origin_site': 0},
                             {'id': 1,
                              'file_id': 0,
                              'line': 3,
                              'col': 161,
                              'end_line': 3,
                              'end_col': 167,
                              'kind': 5,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 0,
                              'slice_abi_stride': 0,
                              'slice_count_arity': 0,
                              'binding': 's',
                              'binding_origin_site': 0}],
                   'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                          'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                'tracked_outstanding_bytes': 0}},
                   'live_allocations': 0,
                   'tracked_outstanding_bytes': 0,
                   'bitmap': '-',
                   'sources': ['sources/primitive-slices/native/primitive_u16.sts'],
                   'source_hashes': {'sources/primitive-slices/native/primitive_u16.sts': '3cdb9a7eb024ee44fd28a4cdc680043b84798ebf09b4e6b1da75c84d7eff4c2f'}},
 'primitive_i32': {'pairs': [],
                   'hits': [],
                   'assertions': 5,
                   'tests': 1,
                   'event_bytes': [20],
                   'allocations': [],
                   'trace': None,
                   'decl_id': 0,
                   'owner': 'case',
                   'static_test_id': 0,
                   'sites': [{'id': 0,
                              'file_id': 0,
                              'line': 3,
                              'col': 20,
                              'end_line': 3,
                              'end_col': 37,
                              'kind': 4,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 5,
                              'slice_abi_stride': 4,
                              'slice_count_arity': 2,
                              'binding': 's',
                              'binding_origin_site': 0},
                             {'id': 1,
                              'file_id': 0,
                              'line': 3,
                              'col': 161,
                              'end_line': 3,
                              'end_col': 167,
                              'kind': 5,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 0,
                              'slice_abi_stride': 0,
                              'slice_count_arity': 0,
                              'binding': 's',
                              'binding_origin_site': 0}],
                   'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                          'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                'tracked_outstanding_bytes': 0}},
                   'live_allocations': 0,
                   'tracked_outstanding_bytes': 0,
                   'bitmap': '-',
                   'sources': ['sources/primitive-slices/native/primitive_i32.sts'],
                   'source_hashes': {'sources/primitive-slices/native/primitive_i32.sts': '79520c94d688a125ae9676dbe2bb5acb5ea8bd1bf3da5fe2644ad339ed0267a8'}},
 'primitive_u32': {'pairs': [],
                   'hits': [],
                   'assertions': 5,
                   'tests': 1,
                   'event_bytes': [20],
                   'allocations': [],
                   'trace': None,
                   'decl_id': 0,
                   'owner': 'case',
                   'static_test_id': 0,
                   'sites': [{'id': 0,
                              'file_id': 0,
                              'line': 3,
                              'col': 20,
                              'end_line': 3,
                              'end_col': 37,
                              'kind': 4,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 6,
                              'slice_abi_stride': 4,
                              'slice_count_arity': 2,
                              'binding': 's',
                              'binding_origin_site': 0},
                             {'id': 1,
                              'file_id': 0,
                              'line': 3,
                              'col': 161,
                              'end_line': 3,
                              'end_col': 167,
                              'kind': 5,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 0,
                              'slice_abi_stride': 0,
                              'slice_count_arity': 0,
                              'binding': 's',
                              'binding_origin_site': 0}],
                   'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                          'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                'tracked_outstanding_bytes': 0}},
                   'live_allocations': 0,
                   'tracked_outstanding_bytes': 0,
                   'bitmap': '-',
                   'sources': ['sources/primitive-slices/native/primitive_u32.sts'],
                   'source_hashes': {'sources/primitive-slices/native/primitive_u32.sts': 'd30899d200ae643bdbb51cdbb1e2befb7c99d33d98e8838de582763e65a94942'}},
 'primitive_i64': {'pairs': [],
                   'hits': [],
                   'assertions': 5,
                   'tests': 1,
                   'event_bytes': [40],
                   'allocations': [],
                   'trace': None,
                   'decl_id': 0,
                   'owner': 'case',
                   'static_test_id': 0,
                   'sites': [{'id': 0,
                              'file_id': 0,
                              'line': 3,
                              'col': 20,
                              'end_line': 3,
                              'end_col': 37,
                              'kind': 4,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 7,
                              'slice_abi_stride': 8,
                              'slice_count_arity': 2,
                              'binding': 's',
                              'binding_origin_site': 0},
                             {'id': 1,
                              'file_id': 0,
                              'line': 3,
                              'col': 161,
                              'end_line': 3,
                              'end_col': 167,
                              'kind': 5,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 0,
                              'slice_abi_stride': 0,
                              'slice_count_arity': 0,
                              'binding': 's',
                              'binding_origin_site': 0}],
                   'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                          'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                'tracked_outstanding_bytes': 0}},
                   'live_allocations': 0,
                   'tracked_outstanding_bytes': 0,
                   'bitmap': '-',
                   'sources': ['sources/primitive-slices/native/primitive_i64.sts'],
                   'source_hashes': {'sources/primitive-slices/native/primitive_i64.sts': '15abeae77a0b36f720837f5aa0405c142210ba37bef169a038e7fbc94bd69cad'}},
 'primitive_u64': {'pairs': [],
                   'hits': [],
                   'assertions': 5,
                   'tests': 1,
                   'event_bytes': [40],
                   'allocations': [],
                   'trace': None,
                   'decl_id': 0,
                   'owner': 'case',
                   'static_test_id': 0,
                   'sites': [{'id': 0,
                              'file_id': 0,
                              'line': 3,
                              'col': 20,
                              'end_line': 3,
                              'end_col': 37,
                              'kind': 4,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 8,
                              'slice_abi_stride': 8,
                              'slice_count_arity': 2,
                              'binding': 's',
                              'binding_origin_site': 0},
                             {'id': 1,
                              'file_id': 0,
                              'line': 3,
                              'col': 161,
                              'end_line': 3,
                              'end_col': 167,
                              'kind': 5,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 0,
                              'slice_abi_stride': 0,
                              'slice_count_arity': 0,
                              'binding': 's',
                              'binding_origin_site': 0}],
                   'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                          'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                'tracked_outstanding_bytes': 0}},
                   'live_allocations': 0,
                   'tracked_outstanding_bytes': 0,
                   'bitmap': '-',
                   'sources': ['sources/primitive-slices/native/primitive_u64.sts'],
                   'source_hashes': {'sources/primitive-slices/native/primitive_u64.sts': '104296b56eaf09fc0e060031721a691ff2af025d32933da1e27f4351242dc9b7'}},
 'primitive_bool': {'pairs': [],
                    'hits': [],
                    'assertions': 5,
                    'tests': 1,
                    'event_bytes': [5],
                    'allocations': [],
                    'trace': None,
                    'decl_id': 0,
                    'owner': 'case',
                    'static_test_id': 0,
                    'sites': [{'id': 0,
                               'file_id': 0,
                               'line': 3,
                               'col': 21,
                               'end_line': 3,
                               'end_col': 39,
                               'kind': 4,
                               'decl_id': 0,
                               'owner': 'case',
                               'static_test_id': 0,
                               'slice_element_kind': 9,
                               'slice_abi_stride': 1,
                               'slice_count_arity': 2,
                               'binding': 's',
                               'binding_origin_site': 0},
                              {'id': 1,
                               'file_id': 0,
                               'line': 3,
                               'col': 177,
                               'end_line': 3,
                               'end_col': 183,
                               'kind': 5,
                               'decl_id': 0,
                               'owner': 'case',
                               'static_test_id': 0,
                               'slice_element_kind': 0,
                               'slice_abi_stride': 0,
                               'slice_count_arity': 0,
                               'binding': 's',
                               'binding_origin_site': 0}],
                    'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                           'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                 'tracked_outstanding_bytes': 0}},
                    'live_allocations': 0,
                    'tracked_outstanding_bytes': 0,
                    'bitmap': '-',
                    'sources': ['sources/primitive-slices/native/primitive_bool.sts'],
                    'source_hashes': {'sources/primitive-slices/native/primitive_bool.sts': '94eea887ce5850557e49a020893a4e28869e50d59810b0f4ff544f1b238ae6bf'}},
 'primitive_f32': {'pairs': [],
                   'hits': [],
                   'assertions': 5,
                   'tests': 1,
                   'event_bytes': [20],
                   'allocations': [],
                   'trace': None,
                   'decl_id': 0,
                   'owner': 'case',
                   'static_test_id': 0,
                   'sites': [{'id': 0,
                              'file_id': 0,
                              'line': 3,
                              'col': 20,
                              'end_line': 3,
                              'end_col': 37,
                              'kind': 4,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 10,
                              'slice_abi_stride': 4,
                              'slice_count_arity': 2,
                              'binding': 's',
                              'binding_origin_site': 0},
                             {'id': 1,
                              'file_id': 0,
                              'line': 3,
                              'col': 169,
                              'end_line': 3,
                              'end_col': 175,
                              'kind': 5,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 0,
                              'slice_abi_stride': 0,
                              'slice_count_arity': 0,
                              'binding': 's',
                              'binding_origin_site': 0}],
                   'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                          'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                'tracked_outstanding_bytes': 0}},
                   'live_allocations': 0,
                   'tracked_outstanding_bytes': 0,
                   'bitmap': '-',
                   'sources': ['sources/primitive-slices/native/primitive_f32.sts'],
                   'source_hashes': {'sources/primitive-slices/native/primitive_f32.sts': '9172f68a09b83c6e40ffd84077958bb39d730c1c8c98beaab273ac173a77b5e0'}},
 'primitive_f64': {'pairs': [],
                   'hits': [],
                   'assertions': 5,
                   'tests': 1,
                   'event_bytes': [40],
                   'allocations': [],
                   'trace': None,
                   'decl_id': 0,
                   'owner': 'case',
                   'static_test_id': 0,
                   'sites': [{'id': 0,
                              'file_id': 0,
                              'line': 3,
                              'col': 20,
                              'end_line': 3,
                              'end_col': 37,
                              'kind': 4,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 11,
                              'slice_abi_stride': 8,
                              'slice_count_arity': 2,
                              'binding': 's',
                              'binding_origin_site': 0},
                             {'id': 1,
                              'file_id': 0,
                              'line': 3,
                              'col': 169,
                              'end_line': 3,
                              'end_col': 175,
                              'kind': 5,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 0,
                              'slice_abi_stride': 0,
                              'slice_count_arity': 0,
                              'binding': 's',
                              'binding_origin_site': 0}],
                   'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                          'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                'tracked_outstanding_bytes': 0}},
                   'live_allocations': 0,
                   'tracked_outstanding_bytes': 0,
                   'bitmap': '-',
                   'sources': ['sources/primitive-slices/native/primitive_f64.sts'],
                   'source_hashes': {'sources/primitive-slices/native/primitive_f64.sts': 'f35de30d93f5b44a834808bda99f245a8507a323a85b887b154c5ed6a4de9bf4'}},
 'count_i8': {'pairs': [],
              'hits': [],
              'assertions': 3,
              'tests': 1,
              'event_bytes': [4],
              'allocations': [],
              'trace': None,
              'decl_id': 0,
              'owner': 'case',
              'static_test_id': 0,
              'sites': [{'id': 0,
                         'file_id': 0,
                         'line': 3,
                         'col': 36,
                         'end_line': 3,
                         'end_col': 50,
                         'kind': 4,
                         'decl_id': 0,
                         'owner': 'case',
                         'static_test_id': 0,
                         'slice_element_kind': 5,
                         'slice_abi_stride': 4,
                         'slice_count_arity': 1,
                         'binding': 's',
                         'binding_origin_site': 0},
                        {'id': 1,
                         'file_id': 0,
                         'line': 3,
                         'col': 122,
                         'end_line': 3,
                         'end_col': 128,
                         'kind': 5,
                         'decl_id': 0,
                         'owner': 'case',
                         'static_test_id': 0,
                         'slice_element_kind': 0,
                         'slice_abi_stride': 0,
                         'slice_count_arity': 0,
                         'binding': 's',
                         'binding_origin_site': 0}],
              'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                     'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                           'tracked_outstanding_bytes': 0}},
              'live_allocations': 0,
              'tracked_outstanding_bytes': 0,
              'bitmap': '-',
              'sources': ['sources/primitive-slices/native/count_i8.sts'],
              'source_hashes': {'sources/primitive-slices/native/count_i8.sts': 'cb6472a6156d4e7f437e85879a6530ea14353cc499cf7c01760d7b92032baa99'}},
 'count_u8': {'pairs': [],
              'hits': [],
              'assertions': 3,
              'tests': 1,
              'event_bytes': [4],
              'allocations': [],
              'trace': None,
              'decl_id': 0,
              'owner': 'case',
              'static_test_id': 0,
              'sites': [{'id': 0,
                         'file_id': 0,
                         'line': 3,
                         'col': 36,
                         'end_line': 3,
                         'end_col': 50,
                         'kind': 4,
                         'decl_id': 0,
                         'owner': 'case',
                         'static_test_id': 0,
                         'slice_element_kind': 5,
                         'slice_abi_stride': 4,
                         'slice_count_arity': 1,
                         'binding': 's',
                         'binding_origin_site': 0},
                        {'id': 1,
                         'file_id': 0,
                         'line': 3,
                         'col': 122,
                         'end_line': 3,
                         'end_col': 128,
                         'kind': 5,
                         'decl_id': 0,
                         'owner': 'case',
                         'static_test_id': 0,
                         'slice_element_kind': 0,
                         'slice_abi_stride': 0,
                         'slice_count_arity': 0,
                         'binding': 's',
                         'binding_origin_site': 0}],
              'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                     'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                           'tracked_outstanding_bytes': 0}},
              'live_allocations': 0,
              'tracked_outstanding_bytes': 0,
              'bitmap': '-',
              'sources': ['sources/primitive-slices/native/count_u8.sts'],
              'source_hashes': {'sources/primitive-slices/native/count_u8.sts': '8f08cb55b7efcf49bad91c4caf34843760178b0b6df0b0dd6f2edce8c2dacab0'}},
 'count_i16': {'pairs': [],
               'hits': [],
               'assertions': 3,
               'tests': 1,
               'event_bytes': [4],
               'allocations': [],
               'trace': None,
               'decl_id': 0,
               'owner': 'case',
               'static_test_id': 0,
               'sites': [{'id': 0,
                          'file_id': 0,
                          'line': 3,
                          'col': 37,
                          'end_line': 3,
                          'end_col': 51,
                          'kind': 4,
                          'decl_id': 0,
                          'owner': 'case',
                          'static_test_id': 0,
                          'slice_element_kind': 5,
                          'slice_abi_stride': 4,
                          'slice_count_arity': 1,
                          'binding': 's',
                          'binding_origin_site': 0},
                         {'id': 1,
                          'file_id': 0,
                          'line': 3,
                          'col': 123,
                          'end_line': 3,
                          'end_col': 129,
                          'kind': 5,
                          'decl_id': 0,
                          'owner': 'case',
                          'static_test_id': 0,
                          'slice_element_kind': 0,
                          'slice_abi_stride': 0,
                          'slice_count_arity': 0,
                          'binding': 's',
                          'binding_origin_site': 0}],
               'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                      'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                            'tracked_outstanding_bytes': 0}},
               'live_allocations': 0,
               'tracked_outstanding_bytes': 0,
               'bitmap': '-',
               'sources': ['sources/primitive-slices/native/count_i16.sts'],
               'source_hashes': {'sources/primitive-slices/native/count_i16.sts': 'f05facca526fc60c569a43fb5e729ffe793f45815cf413f86d6783826f1d6c72'}},
 'count_u16': {'pairs': [],
               'hits': [],
               'assertions': 3,
               'tests': 1,
               'event_bytes': [4],
               'allocations': [],
               'trace': None,
               'decl_id': 0,
               'owner': 'case',
               'static_test_id': 0,
               'sites': [{'id': 0,
                          'file_id': 0,
                          'line': 3,
                          'col': 37,
                          'end_line': 3,
                          'end_col': 51,
                          'kind': 4,
                          'decl_id': 0,
                          'owner': 'case',
                          'static_test_id': 0,
                          'slice_element_kind': 5,
                          'slice_abi_stride': 4,
                          'slice_count_arity': 1,
                          'binding': 's',
                          'binding_origin_site': 0},
                         {'id': 1,
                          'file_id': 0,
                          'line': 3,
                          'col': 123,
                          'end_line': 3,
                          'end_col': 129,
                          'kind': 5,
                          'decl_id': 0,
                          'owner': 'case',
                          'static_test_id': 0,
                          'slice_element_kind': 0,
                          'slice_abi_stride': 0,
                          'slice_count_arity': 0,
                          'binding': 's',
                          'binding_origin_site': 0}],
               'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                      'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                            'tracked_outstanding_bytes': 0}},
               'live_allocations': 0,
               'tracked_outstanding_bytes': 0,
               'bitmap': '-',
               'sources': ['sources/primitive-slices/native/count_u16.sts'],
               'source_hashes': {'sources/primitive-slices/native/count_u16.sts': 'e22f21a92e2490d2bc3370f1702ee936016c71e70e99fb8403b41831243c87b6'}},
 'count_i32': {'pairs': [],
               'hits': [],
               'assertions': 3,
               'tests': 1,
               'event_bytes': [4],
               'allocations': [],
               'trace': None,
               'decl_id': 0,
               'owner': 'case',
               'static_test_id': 0,
               'sites': [{'id': 0,
                          'file_id': 0,
                          'line': 3,
                          'col': 37,
                          'end_line': 3,
                          'end_col': 51,
                          'kind': 4,
                          'decl_id': 0,
                          'owner': 'case',
                          'static_test_id': 0,
                          'slice_element_kind': 5,
                          'slice_abi_stride': 4,
                          'slice_count_arity': 1,
                          'binding': 's',
                          'binding_origin_site': 0},
                         {'id': 1,
                          'file_id': 0,
                          'line': 3,
                          'col': 123,
                          'end_line': 3,
                          'end_col': 129,
                          'kind': 5,
                          'decl_id': 0,
                          'owner': 'case',
                          'static_test_id': 0,
                          'slice_element_kind': 0,
                          'slice_abi_stride': 0,
                          'slice_count_arity': 0,
                          'binding': 's',
                          'binding_origin_site': 0}],
               'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                      'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                            'tracked_outstanding_bytes': 0}},
               'live_allocations': 0,
               'tracked_outstanding_bytes': 0,
               'bitmap': '-',
               'sources': ['sources/primitive-slices/native/count_i32.sts'],
               'source_hashes': {'sources/primitive-slices/native/count_i32.sts': '068fdaabf9d1c994dea6a27ccca4472ff2d6cac7a6f0ed358bae6fa8a5a1346a'}},
 'count_u32': {'pairs': [],
               'hits': [],
               'assertions': 3,
               'tests': 1,
               'event_bytes': [4],
               'allocations': [],
               'trace': None,
               'decl_id': 0,
               'owner': 'case',
               'static_test_id': 0,
               'sites': [{'id': 0,
                          'file_id': 0,
                          'line': 3,
                          'col': 37,
                          'end_line': 3,
                          'end_col': 51,
                          'kind': 4,
                          'decl_id': 0,
                          'owner': 'case',
                          'static_test_id': 0,
                          'slice_element_kind': 5,
                          'slice_abi_stride': 4,
                          'slice_count_arity': 1,
                          'binding': 's',
                          'binding_origin_site': 0},
                         {'id': 1,
                          'file_id': 0,
                          'line': 3,
                          'col': 123,
                          'end_line': 3,
                          'end_col': 129,
                          'kind': 5,
                          'decl_id': 0,
                          'owner': 'case',
                          'static_test_id': 0,
                          'slice_element_kind': 0,
                          'slice_abi_stride': 0,
                          'slice_count_arity': 0,
                          'binding': 's',
                          'binding_origin_site': 0}],
               'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                      'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                            'tracked_outstanding_bytes': 0}},
               'live_allocations': 0,
               'tracked_outstanding_bytes': 0,
               'bitmap': '-',
               'sources': ['sources/primitive-slices/native/count_u32.sts'],
               'source_hashes': {'sources/primitive-slices/native/count_u32.sts': 'a05b21868089edfd025ac4598dcca28e506f86199e433f60a666c26443dda53f'}},
 'count_i64': {'pairs': [],
               'hits': [],
               'assertions': 3,
               'tests': 1,
               'event_bytes': [4],
               'allocations': [],
               'trace': None,
               'decl_id': 0,
               'owner': 'case',
               'static_test_id': 0,
               'sites': [{'id': 0,
                          'file_id': 0,
                          'line': 3,
                          'col': 37,
                          'end_line': 3,
                          'end_col': 51,
                          'kind': 4,
                          'decl_id': 0,
                          'owner': 'case',
                          'static_test_id': 0,
                          'slice_element_kind': 5,
                          'slice_abi_stride': 4,
                          'slice_count_arity': 1,
                          'binding': 's',
                          'binding_origin_site': 0},
                         {'id': 1,
                          'file_id': 0,
                          'line': 3,
                          'col': 123,
                          'end_line': 3,
                          'end_col': 129,
                          'kind': 5,
                          'decl_id': 0,
                          'owner': 'case',
                          'static_test_id': 0,
                          'slice_element_kind': 0,
                          'slice_abi_stride': 0,
                          'slice_count_arity': 0,
                          'binding': 's',
                          'binding_origin_site': 0}],
               'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                      'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                            'tracked_outstanding_bytes': 0}},
               'live_allocations': 0,
               'tracked_outstanding_bytes': 0,
               'bitmap': '-',
               'sources': ['sources/primitive-slices/native/count_i64.sts'],
               'source_hashes': {'sources/primitive-slices/native/count_i64.sts': 'b315fe46b681f9d42358a96a263d8cbc7f5833e49a8fd755ad21f017f718f836'}},
 'count_u64': {'pairs': [],
               'hits': [],
               'assertions': 3,
               'tests': 1,
               'event_bytes': [4],
               'allocations': [],
               'trace': None,
               'decl_id': 0,
               'owner': 'case',
               'static_test_id': 0,
               'sites': [{'id': 0,
                          'file_id': 0,
                          'line': 3,
                          'col': 37,
                          'end_line': 3,
                          'end_col': 51,
                          'kind': 4,
                          'decl_id': 0,
                          'owner': 'case',
                          'static_test_id': 0,
                          'slice_element_kind': 5,
                          'slice_abi_stride': 4,
                          'slice_count_arity': 1,
                          'binding': 's',
                          'binding_origin_site': 0},
                         {'id': 1,
                          'file_id': 0,
                          'line': 3,
                          'col': 123,
                          'end_line': 3,
                          'end_col': 129,
                          'kind': 5,
                          'decl_id': 0,
                          'owner': 'case',
                          'static_test_id': 0,
                          'slice_element_kind': 0,
                          'slice_abi_stride': 0,
                          'slice_count_arity': 0,
                          'binding': 's',
                          'binding_origin_site': 0}],
               'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                      'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                            'tracked_outstanding_bytes': 0}},
               'live_allocations': 0,
               'tracked_outstanding_bytes': 0,
               'bitmap': '-',
               'sources': ['sources/primitive-slices/native/count_u64.sts'],
               'source_hashes': {'sources/primitive-slices/native/count_u64.sts': '1b6e4ca8214f27fa00587bd509334ff1fbe3308c5f5ecd908f7bd9039335e2f3'}},
 'count_bool': {'pairs': [],
                'hits': [],
                'assertions': 3,
                'tests': 1,
                'event_bytes': [4],
                'allocations': [],
                'trace': None,
                'decl_id': 0,
                'owner': 'case',
                'static_test_id': 0,
                'sites': [{'id': 0,
                           'file_id': 0,
                           'line': 3,
                           'col': 41,
                           'end_line': 3,
                           'end_col': 55,
                           'kind': 4,
                           'decl_id': 0,
                           'owner': 'case',
                           'static_test_id': 0,
                           'slice_element_kind': 5,
                           'slice_abi_stride': 4,
                           'slice_count_arity': 1,
                           'binding': 's',
                           'binding_origin_site': 0},
                          {'id': 1,
                           'file_id': 0,
                           'line': 3,
                           'col': 127,
                           'end_line': 3,
                           'end_col': 133,
                           'kind': 5,
                           'decl_id': 0,
                           'owner': 'case',
                           'static_test_id': 0,
                           'slice_element_kind': 0,
                           'slice_abi_stride': 0,
                           'slice_count_arity': 0,
                           'binding': 's',
                           'binding_origin_site': 0}],
                'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                       'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                             'tracked_outstanding_bytes': 0}},
                'live_allocations': 0,
                'tracked_outstanding_bytes': 0,
                'bitmap': '-',
                'sources': ['sources/primitive-slices/native/count_bool.sts'],
                'source_hashes': {'sources/primitive-slices/native/count_bool.sts': '489d827471914026d0d524c80a994db0881f36e71a64cf973f68a9285b8aadde'}},
 'count_unsigned_top_bit': {'pairs': [],
                            'hits': [],
                            'assertions': 2,
                            'tests': 1,
                            'event_bytes': [256],
                            'allocations': [],
                            'trace': None,
                            'decl_id': 0,
                            'owner': 'case',
                            'static_test_id': 0,
                            'sites': [{'id': 0,
                                       'file_id': 0,
                                       'line': 3,
                                       'col': 38,
                                       'end_line': 3,
                                       'end_col': 52,
                                       'kind': 4,
                                       'decl_id': 0,
                                       'owner': 'case',
                                       'static_test_id': 0,
                                       'slice_element_kind': 3,
                                       'slice_abi_stride': 2,
                                       'slice_count_arity': 1,
                                       'binding': 's',
                                       'binding_origin_site': 0},
                                      {'id': 1,
                                       'file_id': 0,
                                       'line': 3,
                                       'col': 104,
                                       'end_line': 3,
                                       'end_col': 110,
                                       'kind': 5,
                                       'decl_id': 0,
                                       'owner': 'case',
                                       'static_test_id': 0,
                                       'slice_element_kind': 0,
                                       'slice_abi_stride': 0,
                                       'slice_count_arity': 0,
                                       'binding': 's',
                                       'binding_origin_site': 0}],
                            'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                                   'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                         'tracked_outstanding_bytes': 0}},
                            'live_allocations': 0,
                            'tracked_outstanding_bytes': 0,
                            'bitmap': '-',
                            'sources': ['sources/primitive-slices/native/count_unsigned_top_bit.sts'],
                            'source_hashes': {'sources/primitive-slices/native/count_unsigned_top_bit.sts': 'd0a867cdd744fa22d4cb644631211d814eed6c285b45c15cda57a79b52c9b48b'}},
 'retained_i32': {'pairs': [],
                  'hits': [],
                  'assertions': 1,
                  'tests': 1,
                  'event_bytes': [20],
                  'allocations': [{'generation': 1,
                                   'domain': 2,
                                   'bytes': 20,
                                   'allocation_site': 0,
                                   'resize_site': 4294967295,
                                   'test': 0}],
                  'trace': None,
                  'decl_id': 0,
                  'owner': 'case',
                  'static_test_id': 0,
                  'sites': [{'id': 0,
                             'file_id': 0,
                             'line': 3,
                             'col': 20,
                             'end_line': 3,
                             'end_col': 37,
                             'kind': 4,
                             'decl_id': 0,
                             'owner': 'case',
                             'static_test_id': 0,
                             'slice_element_kind': 5,
                             'slice_abi_stride': 4,
                             'slice_count_arity': 2,
                             'binding': 's',
                             'binding_origin_site': 0}],
                  'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                         'generated_primitive_slice_backing': {'live_allocations': 1,
                                                                               'tracked_outstanding_bytes': 20}},
                  'live_allocations': 1,
                  'tracked_outstanding_bytes': 20,
                  'bitmap': '-',
                  'sources': ['sources/primitive-slices/native/retained_i32.sts'],
                  'source_hashes': {'sources/primitive-slices/native/retained_i32.sts': '64bbcd74d351b9e8e1f13c9ab8984df2d4a8fcf0e329126d87dfe7ff4cc51a51'}},
 'retained_empty': {'pairs': [],
                    'hits': [],
                    'assertions': 1,
                    'tests': 1,
                    'event_bytes': [12],
                    'allocations': [{'generation': 1,
                                     'domain': 2,
                                     'bytes': 12,
                                     'allocation_site': 0,
                                     'resize_site': 4294967295,
                                     'test': 0}],
                    'trace': None,
                    'decl_id': 0,
                    'owner': 'case',
                    'static_test_id': 0,
                    'sites': [{'id': 0,
                               'file_id': 0,
                               'line': 3,
                               'col': 20,
                               'end_line': 3,
                               'end_col': 37,
                               'kind': 4,
                               'decl_id': 0,
                               'owner': 'case',
                               'static_test_id': 0,
                               'slice_element_kind': 5,
                               'slice_abi_stride': 4,
                               'slice_count_arity': 2,
                               'binding': 's',
                               'binding_origin_site': 0}],
                    'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                           'generated_primitive_slice_backing': {'live_allocations': 1,
                                                                                 'tracked_outstanding_bytes': 12}},
                    'live_allocations': 1,
                    'tracked_outstanding_bytes': 12,
                    'bitmap': '-',
                    'sources': ['sources/primitive-slices/native/retained_empty.sts'],
                    'source_hashes': {'sources/primitive-slices/native/retained_empty.sts': '486354fa7128aef28e86c8606a906fd58b2a39d52a2bc50c3692f442ef7c6b25'}},
 'omitted_i16': {'pairs': [],
                 'hits': [],
                 'assertions': 1,
                 'tests': 1,
                 'event_bytes': [6],
                 'allocations': [{'generation': 1,
                                  'domain': 2,
                                  'bytes': 6,
                                  'allocation_site': 0,
                                  'resize_site': 4294967295,
                                  'test': 0}],
                 'trace': None,
                 'decl_id': 0,
                 'owner': 'case',
                 'static_test_id': 0,
                 'sites': [{'id': 0,
                            'file_id': 0,
                            'line': 3,
                            'col': 20,
                            'end_line': 3,
                            'end_col': 34,
                            'kind': 4,
                            'decl_id': 0,
                            'owner': 'case',
                            'static_test_id': 0,
                            'slice_element_kind': 3,
                            'slice_abi_stride': 2,
                            'slice_count_arity': 1,
                            'binding': 's',
                            'binding_origin_site': 0}],
                 'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                        'generated_primitive_slice_backing': {'live_allocations': 1,
                                                                              'tracked_outstanding_bytes': 6}},
                 'live_allocations': 1,
                 'tracked_outstanding_bytes': 6,
                 'bitmap': '-',
                 'sources': ['sources/primitive-slices/native/omitted_i16.sts'],
                 'source_hashes': {'sources/primitive-slices/native/omitted_i16.sts': '47f0ba3cf1d8e114f268ccb689bf80094a52e591d65790b4ac9553cdd2f1e800'}},
 'zero_omitted': {'pairs': [],
                  'hits': [],
                  'assertions': 2,
                  'tests': 1,
                  'event_bytes': [],
                  'allocations': [],
                  'trace': None,
                  'decl_id': 0,
                  'owner': 'case',
                  'static_test_id': 0,
                  'sites': [{'id': 0,
                             'file_id': 0,
                             'line': 3,
                             'col': 20,
                             'end_line': 3,
                             'end_col': 34,
                             'kind': 4,
                             'decl_id': 0,
                             'owner': 'case',
                             'static_test_id': 0,
                             'slice_element_kind': 5,
                             'slice_abi_stride': 4,
                             'slice_count_arity': 1,
                             'binding': 's',
                             'binding_origin_site': 0},
                            {'id': 1,
                             'file_id': 0,
                             'line': 3,
                             'col': 85,
                             'end_line': 3,
                             'end_col': 91,
                             'kind': 5,
                             'decl_id': 0,
                             'owner': 'case',
                             'static_test_id': 0,
                             'slice_element_kind': 0,
                             'slice_abi_stride': 0,
                             'slice_count_arity': 0,
                             'binding': 's',
                             'binding_origin_site': 0}],
                  'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                         'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                               'tracked_outstanding_bytes': 0}},
                  'live_allocations': 0,
                  'tracked_outstanding_bytes': 0,
                  'bitmap': '-',
                  'sources': ['sources/primitive-slices/native/zero_omitted.sts'],
                  'source_hashes': {'sources/primitive-slices/native/zero_omitted.sts': '024755ac14c2077aca82bb7c0579a03a586d6647d4e9bac06b6d8f38d2f91341'}},
 'zero_explicit': {'pairs': [],
                   'hits': [],
                   'assertions': 2,
                   'tests': 1,
                   'event_bytes': [],
                   'allocations': [],
                   'trace': None,
                   'decl_id': 0,
                   'owner': 'case',
                   'static_test_id': 0,
                   'sites': [{'id': 0,
                              'file_id': 0,
                              'line': 3,
                              'col': 20,
                              'end_line': 3,
                              'end_col': 37,
                              'kind': 4,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 5,
                              'slice_abi_stride': 4,
                              'slice_count_arity': 2,
                              'binding': 's',
                              'binding_origin_site': 0},
                             {'id': 1,
                              'file_id': 0,
                              'line': 3,
                              'col': 88,
                              'end_line': 3,
                              'end_col': 94,
                              'kind': 5,
                              'decl_id': 0,
                              'owner': 'case',
                              'static_test_id': 0,
                              'slice_element_kind': 0,
                              'slice_abi_stride': 0,
                              'slice_count_arity': 0,
                              'binding': 's',
                              'binding_origin_site': 0}],
                   'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                          'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                'tracked_outstanding_bytes': 0}},
                   'live_allocations': 0,
                   'tracked_outstanding_bytes': 0,
                   'bitmap': '-',
                   'sources': ['sources/primitive-slices/native/zero_explicit.sts'],
                   'source_hashes': {'sources/primitive-slices/native/zero_explicit.sts': '5c339d47f79f0fb10176e1dab6c165c9f5c38a34f8e36c458ab56e24fd631e2a'}},
 'mixed_both': {'pairs': [],
                'hits': [],
                'assertions': 2,
                'tests': 1,
                'event_bytes': [7, 20],
                'allocations': [{'generation': 1,
                                 'domain': 1,
                                 'bytes': 7,
                                 'allocation_site': 0,
                                 'resize_site': 4294967295,
                                 'test': 0},
                                {'generation': 2,
                                 'domain': 2,
                                 'bytes': 20,
                                 'allocation_site': 1,
                                 'resize_site': 4294967295,
                                 'test': 0}],
                'trace': None,
                'decl_id': 0,
                'owner': 'case',
                'static_test_id': 0,
                'sites': [{'id': 0,
                           'file_id': 0,
                           'line': 3,
                           'col': 21,
                           'end_line': 3,
                           'end_col': 27,
                           'kind': 1,
                           'decl_id': 0,
                           'owner': 'case',
                           'static_test_id': 0,
                           'slice_element_kind': 0,
                           'slice_abi_stride': 0,
                           'slice_count_arity': 0,
                           'binding': 'p',
                           'binding_origin_site': 0},
                          {'id': 1,
                           'file_id': 0,
                           'line': 3,
                           'col': 45,
                           'end_line': 3,
                           'end_col': 62,
                           'kind': 4,
                           'decl_id': 0,
                           'owner': 'case',
                           'static_test_id': 0,
                           'slice_element_kind': 5,
                           'slice_abi_stride': 4,
                           'slice_count_arity': 2,
                           'binding': 's',
                           'binding_origin_site': 1}],
                'allocation_domains': {'explicit_user_heap': {'live_allocations': 1, 'tracked_outstanding_bytes': 7},
                                       'generated_primitive_slice_backing': {'live_allocations': 1,
                                                                             'tracked_outstanding_bytes': 20}},
                'live_allocations': 2,
                'tracked_outstanding_bytes': 27,
                'bitmap': '-',
                'sources': ['sources/primitive-slices/native/mixed_both.sts'],
                'source_hashes': {'sources/primitive-slices/native/mixed_both.sts': '0ca31d0be5bec29ce3f987fadf3c057e33a26350f167a069e755e252cf3faa68'}},
 'mixed_explicit_only': {'pairs': [],
                         'hits': [],
                         'assertions': 2,
                         'tests': 1,
                         'event_bytes': [7, 20],
                         'allocations': [{'generation': 1,
                                          'domain': 1,
                                          'bytes': 7,
                                          'allocation_site': 0,
                                          'resize_site': 4294967295,
                                          'test': 0}],
                         'trace': None,
                         'decl_id': 0,
                         'owner': 'case',
                         'static_test_id': 0,
                         'sites': [{'id': 0,
                                    'file_id': 0,
                                    'line': 3,
                                    'col': 21,
                                    'end_line': 3,
                                    'end_col': 27,
                                    'kind': 1,
                                    'decl_id': 0,
                                    'owner': 'case',
                                    'static_test_id': 0,
                                    'slice_element_kind': 0,
                                    'slice_abi_stride': 0,
                                    'slice_count_arity': 0,
                                    'binding': 'p',
                                    'binding_origin_site': 0},
                                   {'id': 1,
                                    'file_id': 0,
                                    'line': 3,
                                    'col': 45,
                                    'end_line': 3,
                                    'end_col': 62,
                                    'kind': 4,
                                    'decl_id': 0,
                                    'owner': 'case',
                                    'static_test_id': 0,
                                    'slice_element_kind': 5,
                                    'slice_abi_stride': 4,
                                    'slice_count_arity': 2,
                                    'binding': 's',
                                    'binding_origin_site': 1},
                                   {'id': 2,
                                    'file_id': 0,
                                    'line': 3,
                                    'col': 108,
                                    'end_line': 3,
                                    'end_col': 114,
                                    'kind': 5,
                                    'decl_id': 0,
                                    'owner': 'case',
                                    'static_test_id': 0,
                                    'slice_element_kind': 0,
                                    'slice_abi_stride': 0,
                                    'slice_count_arity': 0,
                                    'binding': 's',
                                    'binding_origin_site': 1}],
                         'allocation_domains': {'explicit_user_heap': {'live_allocations': 1, 'tracked_outstanding_bytes': 7},
                                                'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                      'tracked_outstanding_bytes': 0}},
                         'live_allocations': 1,
                         'tracked_outstanding_bytes': 7,
                         'bitmap': '-',
                         'sources': ['sources/primitive-slices/native/mixed_explicit_only.sts'],
                         'source_hashes': {'sources/primitive-slices/native/mixed_explicit_only.sts': 'e86a4aeeaf50b2cb7a6c9863f0213e1b718fe4354ac6a4d046a7ae6786bd5622'}},
 'mixed_slice_only': {'pairs': [],
                      'hits': [],
                      'assertions': 2,
                      'tests': 1,
                      'event_bytes': [7, 20],
                      'allocations': [{'generation': 2,
                                       'domain': 2,
                                       'bytes': 20,
                                       'allocation_site': 1,
                                       'resize_site': 4294967295,
                                       'test': 0}],
                      'trace': None,
                      'decl_id': 0,
                      'owner': 'case',
                      'static_test_id': 0,
                      'sites': [{'id': 0,
                                 'file_id': 0,
                                 'line': 3,
                                 'col': 21,
                                 'end_line': 3,
                                 'end_col': 27,
                                 'kind': 1,
                                 'decl_id': 0,
                                 'owner': 'case',
                                 'static_test_id': 0,
                                 'slice_element_kind': 0,
                                 'slice_abi_stride': 0,
                                 'slice_count_arity': 0,
                                 'binding': 'p',
                                 'binding_origin_site': 0},
                                {'id': 1,
                                 'file_id': 0,
                                 'line': 3,
                                 'col': 45,
                                 'end_line': 3,
                                 'end_col': 62,
                                 'kind': 4,
                                 'decl_id': 0,
                                 'owner': 'case',
                                 'static_test_id': 0,
                                 'slice_element_kind': 5,
                                 'slice_abi_stride': 4,
                                 'slice_count_arity': 2,
                                 'binding': 's',
                                 'binding_origin_site': 1},
                                {'id': 2,
                                 'file_id': 0,
                                 'line': 3,
                                 'col': 108,
                                 'end_line': 3,
                                 'end_col': 114,
                                 'kind': 3,
                                 'decl_id': 0,
                                 'owner': 'case',
                                 'static_test_id': 0,
                                 'slice_element_kind': 0,
                                 'slice_abi_stride': 0,
                                 'slice_count_arity': 0,
                                 'binding': 'p',
                                 'binding_origin_site': 0}],
                      'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                             'generated_primitive_slice_backing': {'live_allocations': 1,
                                                                                   'tracked_outstanding_bytes': 20}},
                      'live_allocations': 1,
                      'tracked_outstanding_bytes': 20,
                      'bitmap': '-',
                      'sources': ['sources/primitive-slices/native/mixed_slice_only.sts'],
                      'source_hashes': {'sources/primitive-slices/native/mixed_slice_only.sts': 'a05dd4d9a2c1de64d36989603a268a32c26c9c1798858331e53c47c79b9d0806'}},
 'schedule_direct': {'pairs': [[0, 4]],
                     'hits': [[0, 4]],
                     'assertions': 1,
                     'tests': 1,
                     'event_bytes': [8],
                     'allocations': [],
                     'trace': 1,
                     'decl_id': 1,
                     'owner': 'f',
                     'static_test_id': None,
                     'sites': [{'id': 0,
                                'file_id': 0,
                                'line': 4,
                                'col': 20,
                                'end_line': 4,
                                'end_col': 37,
                                'kind': 4,
                                'decl_id': 1,
                                'owner': 'f',
                                'static_test_id': None,
                                'slice_element_kind': 5,
                                'slice_abi_stride': 4,
                                'slice_count_arity': 2,
                                'binding': 's',
                                'binding_origin_site': 0},
                               {'id': 1,
                                'file_id': 0,
                                'line': 4,
                                'col': 61,
                                'end_line': 4,
                                'end_col': 67,
                                'kind': 5,
                                'decl_id': 1,
                                'owner': 'f',
                                'static_test_id': None,
                                'slice_element_kind': 0,
                                'slice_abi_stride': 0,
                                'slice_count_arity': 0,
                                'binding': 's',
                                'binding_origin_site': 0}],
                     'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                            'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                  'tracked_outstanding_bytes': 0}},
                     'live_allocations': 0,
                     'tracked_outstanding_bytes': 0,
                     'bitmap': '01',
                     'sources': ['sources/primitive-slices/native/schedule_direct.sts'],
                     'source_hashes': {'sources/primitive-slices/native/schedule_direct.sts': 'd48956e0334bb398e0c47c634dbccd21f39724faeb91e24c849c07f2f00c104d'}},
 'schedule_defer_fallthrough': {'pairs': [[0, 4]],
                                'hits': [[0, 4]],
                                'assertions': 1,
                                'tests': 1,
                                'event_bytes': [8],
                                'allocations': [],
                                'trace': 1,
                                'decl_id': 1,
                                'owner': 'f',
                                'static_test_id': None,
                                'sites': [{'id': 0,
                                           'file_id': 0,
                                           'line': 4,
                                           'col': 20,
                                           'end_line': 4,
                                           'end_col': 37,
                                           'kind': 4,
                                           'decl_id': 1,
                                           'owner': 'f',
                                           'static_test_id': None,
                                           'slice_element_kind': 5,
                                           'slice_abi_stride': 4,
                                           'slice_count_arity': 2,
                                           'binding': 's',
                                           'binding_origin_site': 0},
                                          {'id': 1,
                                           'file_id': 0,
                                           'line': 4,
                                           'col': 48,
                                           'end_line': 4,
                                           'end_col': 54,
                                           'kind': 5,
                                           'decl_id': 1,
                                           'owner': 'f',
                                           'static_test_id': None,
                                           'slice_element_kind': 0,
                                           'slice_abi_stride': 0,
                                           'slice_count_arity': 0,
                                           'binding': 's',
                                           'binding_origin_site': 0}],
                                'allocation_domains': {'explicit_user_heap': {'live_allocations': 0,
                                                                              'tracked_outstanding_bytes': 0},
                                                       'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                             'tracked_outstanding_bytes': 0}},
                                'live_allocations': 0,
                                'tracked_outstanding_bytes': 0,
                                'bitmap': '01',
                                'sources': ['sources/primitive-slices/native/schedule_defer_fallthrough.sts'],
                                'source_hashes': {'sources/primitive-slices/native/schedule_defer_fallthrough.sts': '6a79991613c4297f4ee4bc3f102de188d1e20dd6a89f6931d89fd4427ce70fbe'}},
 'schedule_defer_return': {'pairs': [[0, 4]],
                           'hits': [[0, 4]],
                           'assertions': 1,
                           'tests': 1,
                           'event_bytes': [8],
                           'allocations': [],
                           'trace': 1,
                           'decl_id': 1,
                           'owner': 'f',
                           'static_test_id': None,
                           'sites': [{'id': 0,
                                      'file_id': 0,
                                      'line': 4,
                                      'col': 20,
                                      'end_line': 4,
                                      'end_col': 37,
                                      'kind': 4,
                                      'decl_id': 1,
                                      'owner': 'f',
                                      'static_test_id': None,
                                      'slice_element_kind': 5,
                                      'slice_abi_stride': 4,
                                      'slice_count_arity': 2,
                                      'binding': 's',
                                      'binding_origin_site': 0},
                                     {'id': 1,
                                      'file_id': 0,
                                      'line': 4,
                                      'col': 48,
                                      'end_line': 4,
                                      'end_col': 54,
                                      'kind': 5,
                                      'decl_id': 1,
                                      'owner': 'f',
                                      'static_test_id': None,
                                      'slice_element_kind': 0,
                                      'slice_abi_stride': 0,
                                      'slice_count_arity': 0,
                                      'binding': 's',
                                      'binding_origin_site': 0}],
                           'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                                  'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                        'tracked_outstanding_bytes': 0}},
                           'live_allocations': 0,
                           'tracked_outstanding_bytes': 0,
                           'bitmap': '01',
                           'sources': ['sources/primitive-slices/native/schedule_defer_return.sts'],
                           'source_hashes': {'sources/primitive-slices/native/schedule_defer_return.sts': '47585f73860a5d5b7c49a4b712ca7d480c5fa50208b8f61622a98c1813288c63'}},
 'schedule_defer_nested': {'pairs': [[0, 4]],
                           'hits': [[0, 4]],
                           'assertions': 1,
                           'tests': 1,
                           'event_bytes': [8, 6],
                           'allocations': [],
                           'trace': 5,
                           'decl_id': 1,
                           'owner': 'f',
                           'static_test_id': None,
                           'sites': [{'id': 0,
                                      'file_id': 0,
                                      'line': 4,
                                      'col': 20,
                                      'end_line': 4,
                                      'end_col': 37,
                                      'kind': 4,
                                      'decl_id': 1,
                                      'owner': 'f',
                                      'static_test_id': None,
                                      'slice_element_kind': 5,
                                      'slice_abi_stride': 4,
                                      'slice_count_arity': 2,
                                      'binding': 's',
                                      'binding_origin_site': 0},
                                     {'id': 1,
                                      'file_id': 0,
                                      'line': 4,
                                      'col': 48,
                                      'end_line': 4,
                                      'end_col': 54,
                                      'kind': 5,
                                      'decl_id': 1,
                                      'owner': 'f',
                                      'static_test_id': None,
                                      'slice_element_kind': 0,
                                      'slice_abi_stride': 0,
                                      'slice_count_arity': 0,
                                      'binding': 's',
                                      'binding_origin_site': 0},
                                     {'id': 2,
                                      'file_id': 0,
                                      'line': 4,
                                      'col': 76,
                                      'end_line': 4,
                                      'end_col': 93,
                                      'kind': 4,
                                      'decl_id': 1,
                                      'owner': 'f',
                                      'static_test_id': None,
                                      'slice_element_kind': 3,
                                      'slice_abi_stride': 2,
                                      'slice_count_arity': 2,
                                      'binding': 't',
                                      'binding_origin_site': 2},
                                     {'id': 3,
                                      'file_id': 0,
                                      'line': 4,
                                      'col': 104,
                                      'end_line': 4,
                                      'end_col': 110,
                                      'kind': 5,
                                      'decl_id': 1,
                                      'owner': 'f',
                                      'static_test_id': None,
                                      'slice_element_kind': 0,
                                      'slice_abi_stride': 0,
                                      'slice_count_arity': 0,
                                      'binding': 't',
                                      'binding_origin_site': 2}],
                           'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                                  'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                        'tracked_outstanding_bytes': 0}},
                           'live_allocations': 0,
                           'tracked_outstanding_bytes': 0,
                           'bitmap': '01',
                           'sources': ['sources/primitive-slices/native/schedule_defer_nested.sts'],
                           'source_hashes': {'sources/primitive-slices/native/schedule_defer_nested.sts': 'e45a8b11daa204f0082e6e7a57d6283d4226afbb0eee71d756eea40dbe1ec835'}},
 'schedule_defer_continue': {'pairs': [[0, 4]],
                             'hits': [[0, 4]],
                             'assertions': 1,
                             'tests': 1,
                             'event_bytes': [4, 4, 4],
                             'allocations': [],
                             'trace': 3,
                             'decl_id': 1,
                             'owner': 'f',
                             'static_test_id': None,
                             'sites': [{'id': 0,
                                        'file_id': 0,
                                        'line': 4,
                                        'col': 53,
                                        'end_line': 4,
                                        'end_col': 67,
                                        'kind': 4,
                                        'decl_id': 1,
                                        'owner': 'f',
                                        'static_test_id': None,
                                        'slice_element_kind': 5,
                                        'slice_abi_stride': 4,
                                        'slice_count_arity': 1,
                                        'binding': 's',
                                        'binding_origin_site': 0},
                                       {'id': 1,
                                        'file_id': 0,
                                        'line': 4,
                                        'col': 78,
                                        'end_line': 4,
                                        'end_col': 84,
                                        'kind': 5,
                                        'decl_id': 1,
                                        'owner': 'f',
                                        'static_test_id': None,
                                        'slice_element_kind': 0,
                                        'slice_abi_stride': 0,
                                        'slice_count_arity': 0,
                                        'binding': 's',
                                        'binding_origin_site': 0}],
                             'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                                    'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                          'tracked_outstanding_bytes': 0}},
                             'live_allocations': 0,
                             'tracked_outstanding_bytes': 0,
                             'bitmap': '01',
                             'sources': ['sources/primitive-slices/native/schedule_defer_continue.sts'],
                             'source_hashes': {'sources/primitive-slices/native/schedule_defer_continue.sts': '880ef333b52193cac79551a03021baabeb8f799b439e176acfe88ba4cf0afac3'}},
 'schedule_defer_break': {'pairs': [[0, 4]],
                          'hits': [[0, 4]],
                          'assertions': 1,
                          'tests': 1,
                          'event_bytes': [4],
                          'allocations': [],
                          'trace': 1,
                          'decl_id': 1,
                          'owner': 'f',
                          'static_test_id': None,
                          'sites': [{'id': 0,
                                     'file_id': 0,
                                     'line': 4,
                                     'col': 26,
                                     'end_line': 4,
                                     'end_col': 40,
                                     'kind': 4,
                                     'decl_id': 1,
                                     'owner': 'f',
                                     'static_test_id': None,
                                     'slice_element_kind': 5,
                                     'slice_abi_stride': 4,
                                     'slice_count_arity': 1,
                                     'binding': 's',
                                     'binding_origin_site': 0},
                                    {'id': 1,
                                     'file_id': 0,
                                     'line': 4,
                                     'col': 51,
                                     'end_line': 4,
                                     'end_col': 57,
                                     'kind': 5,
                                     'decl_id': 1,
                                     'owner': 'f',
                                     'static_test_id': None,
                                     'slice_element_kind': 0,
                                     'slice_abi_stride': 0,
                                     'slice_count_arity': 0,
                                     'binding': 's',
                                     'binding_origin_site': 0}],
                          'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                                 'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                       'tracked_outstanding_bytes': 0}},
                          'live_allocations': 0,
                          'tracked_outstanding_bytes': 0,
                          'bitmap': '01',
                          'sources': ['sources/primitive-slices/native/schedule_defer_break.sts'],
                          'source_hashes': {'sources/primitive-slices/native/schedule_defer_break.sts': '531bf3ce44b35e6453e431486ac3057266ed5f19d65a1f54717e86aa63a878b8'}},
 'schedule_defer_nested_exits': {'pairs': [[0, 4]],
                                 'hits': [[0, 4]],
                                 'assertions': 1,
                                 'tests': 1,
                                 'event_bytes': [2, 6, 16, 6, 16],
                                 'allocations': [],
                                 'trace': 2,
                                 'decl_id': 1,
                                 'owner': 'f',
                                 'static_test_id': None,
                                 'sites': [{'id': 0,
                                            'file_id': 0,
                                            'line': 4,
                                            'col': 23,
                                            'end_line': 4,
                                            'end_col': 39,
                                            'kind': 4,
                                            'decl_id': 1,
                                            'owner': 'f',
                                            'static_test_id': None,
                                            'slice_element_kind': 2,
                                            'slice_abi_stride': 1,
                                            'slice_count_arity': 2,
                                            'binding': 'outer',
                                            'binding_origin_site': 0},
                                           {'id': 1,
                                            'file_id': 0,
                                            'line': 4,
                                            'col': 50,
                                            'end_line': 4,
                                            'end_col': 60,
                                            'kind': 5,
                                            'decl_id': 1,
                                            'owner': 'f',
                                            'static_test_id': None,
                                            'slice_element_kind': 0,
                                            'slice_abi_stride': 0,
                                            'slice_count_arity': 0,
                                            'binding': 'outer',
                                            'binding_origin_site': 0},
                                           {'id': 2,
                                            'file_id': 0,
                                            'line': 4,
                                            'col': 105,
                                            'end_line': 4,
                                            'end_col': 122,
                                            'kind': 4,
                                            'decl_id': 1,
                                            'owner': 'f',
                                            'static_test_id': None,
                                            'slice_element_kind': 3,
                                            'slice_abi_stride': 2,
                                            'slice_count_arity': 2,
                                            'binding': 'middle',
                                            'binding_origin_site': 2},
                                           {'id': 3,
                                            'file_id': 0,
                                            'line': 4,
                                            'col': 133,
                                            'end_line': 4,
                                            'end_col': 144,
                                            'kind': 5,
                                            'decl_id': 1,
                                            'owner': 'f',
                                            'static_test_id': None,
                                            'slice_element_kind': 0,
                                            'slice_abi_stride': 0,
                                            'slice_count_arity': 0,
                                            'binding': 'middle',
                                            'binding_origin_site': 2},
                                           {'id': 4,
                                            'file_id': 0,
                                            'line': 4,
                                            'col': 170,
                                            'end_line': 4,
                                            'end_col': 187,
                                            'kind': 4,
                                            'decl_id': 1,
                                            'owner': 'f',
                                            'static_test_id': None,
                                            'slice_element_kind': 5,
                                            'slice_abi_stride': 4,
                                            'slice_count_arity': 2,
                                            'binding': 'inner',
                                            'binding_origin_site': 4},
                                           {'id': 5,
                                            'file_id': 0,
                                            'line': 4,
                                            'col': 198,
                                            'end_line': 4,
                                            'end_col': 208,
                                            'kind': 5,
                                            'decl_id': 1,
                                            'owner': 'f',
                                            'static_test_id': None,
                                            'slice_element_kind': 0,
                                            'slice_abi_stride': 0,
                                            'slice_count_arity': 0,
                                            'binding': 'inner',
                                            'binding_origin_site': 4}],
                                 'allocation_domains': {'explicit_user_heap': {'live_allocations': 0,
                                                                               'tracked_outstanding_bytes': 0},
                                                        'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                              'tracked_outstanding_bytes': 0}},
                                 'live_allocations': 0,
                                 'tracked_outstanding_bytes': 0,
                                 'bitmap': '01',
                                 'sources': ['sources/primitive-slices/native/schedule_defer_nested_exits.sts'],
                                 'source_hashes': {'sources/primitive-slices/native/schedule_defer_nested_exits.sts': '31817278d0cf0c0a9d1453893cb4a455155ef1c29f54f32e6562ee10a28a2fa7'}},
 'schedule_direct_bypass_return': {'pairs': [[0, 4]],
                                   'hits': [[0, 4]],
                                   'assertions': 1,
                                   'tests': 1,
                                   'event_bytes': [20],
                                   'allocations': [{'generation': 1,
                                                    'domain': 2,
                                                    'bytes': 20,
                                                    'allocation_site': 0,
                                                    'resize_site': 4294967295,
                                                    'test': 0}],
                                   'trace': 1,
                                   'decl_id': 1,
                                   'owner': 'f',
                                   'static_test_id': None,
                                   'sites': [{'id': 0,
                                              'file_id': 0,
                                              'line': 4,
                                              'col': 20,
                                              'end_line': 4,
                                              'end_col': 37,
                                              'kind': 4,
                                              'decl_id': 1,
                                              'owner': 'f',
                                              'static_test_id': None,
                                              'slice_element_kind': 5,
                                              'slice_abi_stride': 4,
                                              'slice_count_arity': 2,
                                              'binding': 's',
                                              'binding_origin_site': 0},
                                             {'id': 1,
                                              'file_id': 0,
                                              'line': 4,
                                              'col': 56,
                                              'end_line': 4,
                                              'end_col': 62,
                                              'kind': 5,
                                              'decl_id': 1,
                                              'owner': 'f',
                                              'static_test_id': None,
                                              'slice_element_kind': 0,
                                              'slice_abi_stride': 0,
                                              'slice_count_arity': 0,
                                              'binding': 's',
                                              'binding_origin_site': 0}],
                                   'allocation_domains': {'explicit_user_heap': {'live_allocations': 0,
                                                                                 'tracked_outstanding_bytes': 0},
                                                          'generated_primitive_slice_backing': {'live_allocations': 1,
                                                                                                'tracked_outstanding_bytes': 20}},
                                   'live_allocations': 1,
                                   'tracked_outstanding_bytes': 20,
                                   'bitmap': '01',
                                   'sources': ['sources/primitive-slices/native/schedule_direct_bypass_return.sts'],
                                   'source_hashes': {'sources/primitive-slices/native/schedule_direct_bypass_return.sts': '0b3de5516b152943b3f9880e3254962d08c223108ad9188f8509841300885aaa'}},
 'schedule_direct_bypass_break': {'pairs': [[0, 4]],
                                  'hits': [[0, 4]],
                                  'assertions': 1,
                                  'tests': 1,
                                  'event_bytes': [20],
                                  'allocations': [{'generation': 1,
                                                   'domain': 2,
                                                   'bytes': 20,
                                                   'allocation_site': 0,
                                                   'resize_site': 4294967295,
                                                   'test': 0}],
                                  'trace': 1,
                                  'decl_id': 1,
                                  'owner': 'f',
                                  'static_test_id': None,
                                  'sites': [{'id': 0,
                                             'file_id': 0,
                                             'line': 4,
                                             'col': 26,
                                             'end_line': 4,
                                             'end_col': 43,
                                             'kind': 4,
                                             'decl_id': 1,
                                             'owner': 'f',
                                             'static_test_id': None,
                                             'slice_element_kind': 5,
                                             'slice_abi_stride': 4,
                                             'slice_count_arity': 2,
                                             'binding': 's',
                                             'binding_origin_site': 0},
                                            {'id': 1,
                                             'file_id': 0,
                                             'line': 4,
                                             'col': 64,
                                             'end_line': 4,
                                             'end_col': 70,
                                             'kind': 5,
                                             'decl_id': 1,
                                             'owner': 'f',
                                             'static_test_id': None,
                                             'slice_element_kind': 0,
                                             'slice_abi_stride': 0,
                                             'slice_count_arity': 0,
                                             'binding': 's',
                                             'binding_origin_site': 0}],
                                  'allocation_domains': {'explicit_user_heap': {'live_allocations': 0,
                                                                                'tracked_outstanding_bytes': 0},
                                                         'generated_primitive_slice_backing': {'live_allocations': 1,
                                                                                               'tracked_outstanding_bytes': 20}},
                                  'live_allocations': 1,
                                  'tracked_outstanding_bytes': 20,
                                  'bitmap': '01',
                                  'sources': ['sources/primitive-slices/native/schedule_direct_bypass_break.sts'],
                                  'source_hashes': {'sources/primitive-slices/native/schedule_direct_bypass_break.sts': '3c0e5d9f61cdbfb1e7e19243b7fbd7e1f3d245b01db515c8439812ea106ecc71'}},
 'schedule_direct_bypass_continue': {'pairs': [[0, 4]],
                                     'hits': [[0, 4]],
                                     'assertions': 1,
                                     'tests': 1,
                                     'event_bytes': [20, 20],
                                     'allocations': [{'generation': 1,
                                                      'domain': 2,
                                                      'bytes': 20,
                                                      'allocation_site': 0,
                                                      'resize_site': 4294967295,
                                                      'test': 0},
                                                     {'generation': 2,
                                                      'domain': 2,
                                                      'bytes': 20,
                                                      'allocation_site': 0,
                                                      'resize_site': 4294967295,
                                                      'test': 0}],
                                     'trace': 2,
                                     'decl_id': 1,
                                     'owner': 'f',
                                     'static_test_id': None,
                                     'sites': [{'id': 0,
                                                'file_id': 0,
                                                'line': 4,
                                                'col': 40,
                                                'end_line': 4,
                                                'end_col': 57,
                                                'kind': 4,
                                                'decl_id': 1,
                                                'owner': 'f',
                                                'static_test_id': None,
                                                'slice_element_kind': 5,
                                                'slice_abi_stride': 4,
                                                'slice_count_arity': 2,
                                                'binding': 's',
                                                'binding_origin_site': 0},
                                               {'id': 1,
                                                'file_id': 0,
                                                'line': 4,
                                                'col': 82,
                                                'end_line': 4,
                                                'end_col': 88,
                                                'kind': 5,
                                                'decl_id': 1,
                                                'owner': 'f',
                                                'static_test_id': None,
                                                'slice_element_kind': 0,
                                                'slice_abi_stride': 0,
                                                'slice_count_arity': 0,
                                                'binding': 's',
                                                'binding_origin_site': 0}],
                                     'allocation_domains': {'explicit_user_heap': {'live_allocations': 0,
                                                                                   'tracked_outstanding_bytes': 0},
                                                            'generated_primitive_slice_backing': {'live_allocations': 2,
                                                                                                  'tracked_outstanding_bytes': 40}},
                                     'live_allocations': 2,
                                     'tracked_outstanding_bytes': 40,
                                     'bitmap': '01',
                                     'sources': ['sources/primitive-slices/native/schedule_direct_bypass_continue.sts'],
                                     'source_hashes': {'sources/primitive-slices/native/schedule_direct_bypass_continue.sts': '760e0c0ba3c6a5e7dc97dd70a1876bd5b1ec0beebca326f753a8b1e4932b88cc'}},
 'schedule_shadow_defer': {'pairs': [[0, 4]],
                           'hits': [[0, 4]],
                           'assertions': 1,
                           'tests': 1,
                           'event_bytes': [20, 6],
                           'allocations': [],
                           'trace': 3,
                           'decl_id': 1,
                           'owner': 'f',
                           'static_test_id': None,
                           'sites': [{'id': 0,
                                      'file_id': 0,
                                      'line': 4,
                                      'col': 20,
                                      'end_line': 4,
                                      'end_col': 37,
                                      'kind': 4,
                                      'decl_id': 1,
                                      'owner': 'f',
                                      'static_test_id': None,
                                      'slice_element_kind': 5,
                                      'slice_abi_stride': 4,
                                      'slice_count_arity': 2,
                                      'binding': 's',
                                      'binding_origin_site': 0},
                                     {'id': 1,
                                      'file_id': 0,
                                      'line': 4,
                                      'col': 48,
                                      'end_line': 4,
                                      'end_col': 54,
                                      'kind': 5,
                                      'decl_id': 1,
                                      'owner': 'f',
                                      'static_test_id': None,
                                      'slice_element_kind': 0,
                                      'slice_abi_stride': 0,
                                      'slice_count_arity': 0,
                                      'binding': 's',
                                      'binding_origin_site': 0},
                                     {'id': 2,
                                      'file_id': 0,
                                      'line': 4,
                                      'col': 76,
                                      'end_line': 4,
                                      'end_col': 99,
                                      'kind': 4,
                                      'decl_id': 1,
                                      'owner': 'f',
                                      'static_test_id': None,
                                      'slice_element_kind': 3,
                                      'slice_abi_stride': 2,
                                      'slice_count_arity': 2,
                                      'binding': 's',
                                      'binding_origin_site': 2},
                                     {'id': 3,
                                      'file_id': 0,
                                      'line': 4,
                                      'col': 110,
                                      'end_line': 4,
                                      'end_col': 116,
                                      'kind': 5,
                                      'decl_id': 1,
                                      'owner': 'f',
                                      'static_test_id': None,
                                      'slice_element_kind': 0,
                                      'slice_abi_stride': 0,
                                      'slice_count_arity': 0,
                                      'binding': 's',
                                      'binding_origin_site': 2}],
                           'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                                  'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                        'tracked_outstanding_bytes': 0}},
                           'live_allocations': 0,
                           'tracked_outstanding_bytes': 0,
                           'bitmap': '01',
                           'sources': ['sources/primitive-slices/native/schedule_shadow_defer.sts'],
                           'source_hashes': {'sources/primitive-slices/native/schedule_shadow_defer.sts': 'a301487cbb2fd7ef93014e0a7bfdecc98cd188209cd1d6d47f3418167978f585'}},
 'schedule_shadow_siblings': {'pairs': [[0, 4]],
                              'hits': [[0, 4]],
                              'assertions': 1,
                              'tests': 1,
                              'event_bytes': [8, 6],
                              'allocations': [],
                              'trace': 5,
                              'decl_id': 1,
                              'owner': 'f',
                              'static_test_id': None,
                              'sites': [{'id': 0,
                                         'file_id': 0,
                                         'line': 4,
                                         'col': 22,
                                         'end_line': 4,
                                         'end_col': 39,
                                         'kind': 4,
                                         'decl_id': 1,
                                         'owner': 'f',
                                         'static_test_id': None,
                                         'slice_element_kind': 5,
                                         'slice_abi_stride': 4,
                                         'slice_count_arity': 2,
                                         'binding': 's',
                                         'binding_origin_site': 0},
                                        {'id': 1,
                                         'file_id': 0,
                                         'line': 4,
                                         'col': 50,
                                         'end_line': 4,
                                         'end_col': 56,
                                         'kind': 5,
                                         'decl_id': 1,
                                         'owner': 'f',
                                         'static_test_id': None,
                                         'slice_element_kind': 0,
                                         'slice_abi_stride': 0,
                                         'slice_count_arity': 0,
                                         'binding': 's',
                                         'binding_origin_site': 0},
                                        {'id': 2,
                                         'file_id': 0,
                                         'line': 4,
                                         'col': 98,
                                         'end_line': 4,
                                         'end_col': 115,
                                         'kind': 4,
                                         'decl_id': 1,
                                         'owner': 'f',
                                         'static_test_id': None,
                                         'slice_element_kind': 3,
                                         'slice_abi_stride': 2,
                                         'slice_count_arity': 2,
                                         'binding': 's',
                                         'binding_origin_site': 2},
                                        {'id': 3,
                                         'file_id': 0,
                                         'line': 4,
                                         'col': 126,
                                         'end_line': 4,
                                         'end_col': 132,
                                         'kind': 5,
                                         'decl_id': 1,
                                         'owner': 'f',
                                         'static_test_id': None,
                                         'slice_element_kind': 0,
                                         'slice_abi_stride': 0,
                                         'slice_count_arity': 0,
                                         'binding': 's',
                                         'binding_origin_site': 2}],
                              'allocation_domains': {'explicit_user_heap': {'live_allocations': 0,
                                                                            'tracked_outstanding_bytes': 0},
                                                     'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                           'tracked_outstanding_bytes': 0}},
                              'live_allocations': 0,
                              'tracked_outstanding_bytes': 0,
                              'bitmap': '01',
                              'sources': ['sources/primitive-slices/native/schedule_shadow_siblings.sts'],
                              'source_hashes': {'sources/primitive-slices/native/schedule_shadow_siblings.sts': '4c8b90db271549c97c3abd305034d84ac9c325d7e6285eb8136f833663457241'}},
 'nested_scalar': {'pairs': [[0, 2], [0, 4], [0, 5], [0, 6], [0, 7], [0, 8], [0, 9]],
                   'hits': [[0, 2], [0, 4], [0, 5], [0, 6], [0, 7], [0, 8], [0, 9]],
                   'assertions': 1,
                   'tests': 1,
                   'event_bytes': [20, 8],
                   'allocations': [],
                   'trace': None,
                   'decl_id': 1,
                   'owner': 'work',
                   'static_test_id': None,
                   'sites': [{'id': 0,
                              'file_id': 0,
                              'line': 4,
                              'col': 20,
                              'end_line': 4,
                              'end_col': 37,
                              'kind': 4,
                              'decl_id': 1,
                              'owner': 'work',
                              'static_test_id': None,
                              'slice_element_kind': 5,
                              'slice_abi_stride': 4,
                              'slice_count_arity': 2,
                              'binding': 's',
                              'binding_origin_site': 0},
                             {'id': 1,
                              'file_id': 0,
                              'line': 5,
                              'col': 13,
                              'end_line': 5,
                              'end_col': 19,
                              'kind': 5,
                              'decl_id': 1,
                              'owner': 'work',
                              'static_test_id': None,
                              'slice_element_kind': 0,
                              'slice_abi_stride': 0,
                              'slice_count_arity': 0,
                              'binding': 's',
                              'binding_origin_site': 0},
                             {'id': 2,
                              'file_id': 0,
                              'line': 7,
                              'col': 20,
                              'end_line': 7,
                              'end_col': 61,
                              'kind': 4,
                              'decl_id': 1,
                              'owner': 'work',
                              'static_test_id': None,
                              'slice_element_kind': 3,
                              'slice_abi_stride': 2,
                              'slice_count_arity': 2,
                              'binding': 't',
                              'binding_origin_site': 2},
                             {'id': 3,
                              'file_id': 0,
                              'line': 8,
                              'col': 13,
                              'end_line': 8,
                              'end_col': 19,
                              'kind': 5,
                              'decl_id': 1,
                              'owner': 'work',
                              'static_test_id': None,
                              'slice_element_kind': 0,
                              'slice_abi_stride': 0,
                              'slice_count_arity': 0,
                              'binding': 't',
                              'binding_origin_site': 2}],
                   'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                          'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                'tracked_outstanding_bytes': 0}},
                   'live_allocations': 0,
                   'tracked_outstanding_bytes': 0,
                   'bitmap': '7f',
                   'sources': ['sources/primitive-slices/native/nested_scalar.sts'],
                   'source_hashes': {'sources/primitive-slices/native/nested_scalar.sts': '1082856bf8eef1366033409e24d4d242d54c7b1664a3ebe5868199d9efc72c8e'}},
 'no_assertions': {'pairs': [[0, 3], [0, 4]],
                   'hits': [[0, 3], [0, 4]],
                   'assertions': 0,
                   'tests': 1,
                   'event_bytes': [8],
                   'allocations': [],
                   'trace': None,
                   'decl_id': 0,
                   'owner': 'work',
                   'static_test_id': None,
                   'sites': [{'id': 0,
                              'file_id': 0,
                              'line': 3,
                              'col': 20,
                              'end_line': 3,
                              'end_col': 37,
                              'kind': 4,
                              'decl_id': 0,
                              'owner': 'work',
                              'static_test_id': None,
                              'slice_element_kind': 5,
                              'slice_abi_stride': 4,
                              'slice_count_arity': 2,
                              'binding': 's',
                              'binding_origin_site': 0},
                             {'id': 1,
                              'file_id': 0,
                              'line': 4,
                              'col': 5,
                              'end_line': 4,
                              'end_col': 11,
                              'kind': 5,
                              'decl_id': 0,
                              'owner': 'work',
                              'static_test_id': None,
                              'slice_element_kind': 0,
                              'slice_abi_stride': 0,
                              'slice_count_arity': 0,
                              'binding': 's',
                              'binding_origin_site': 0}],
                   'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                          'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                                'tracked_outstanding_bytes': 0}},
                   'live_allocations': 0,
                   'tracked_outstanding_bytes': 0,
                   'bitmap': '03',
                   'sources': ['sources/primitive-slices/native/no_assertions.sts'],
                   'source_hashes': {'sources/primitive-slices/native/no_assertions.sts': 'f2b9dd63799c68021f227c94da46c377c4f549ceddbe3f630764d65c24529c26'}},
 'coverage': {'pairs': [[0, 3], [0, 4], [0, 5], [0, 6], [0, 9], [0, 10]],
              'hits': [[0, 3], [0, 4], [0, 5], [0, 6]],
              'assertions': 1,
              'tests': 1,
              'event_bytes': [20],
              'allocations': [],
              'trace': None,
              'decl_id': 0,
              'owner': 'work',
              'static_test_id': None,
              'sites': [{'id': 0,
                         'file_id': 0,
                         'line': 3,
                         'col': 20,
                         'end_line': 3,
                         'end_col': 37,
                         'kind': 4,
                         'decl_id': 0,
                         'owner': 'work',
                         'static_test_id': None,
                         'slice_element_kind': 5,
                         'slice_abi_stride': 4,
                         'slice_count_arity': 2,
                         'binding': 's',
                         'binding_origin_site': 0},
                        {'id': 1,
                         'file_id': 0,
                         'line': 4,
                         'col': 13,
                         'end_line': 4,
                         'end_col': 19,
                         'kind': 5,
                         'decl_id': 0,
                         'owner': 'work',
                         'static_test_id': None,
                         'slice_element_kind': 0,
                         'slice_abi_stride': 0,
                         'slice_count_arity': 0,
                         'binding': 's',
                         'binding_origin_site': 0},
                        {'id': 2,
                         'file_id': 0,
                         'line': 9,
                         'col': 23,
                         'end_line': 9,
                         'end_col': 40,
                         'kind': 4,
                         'decl_id': 1,
                         'owner': 'never',
                         'static_test_id': None,
                         'slice_element_kind': 3,
                         'slice_abi_stride': 2,
                         'slice_count_arity': 2,
                         'binding': 'cold',
                         'binding_origin_site': 2}],
              'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                     'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                           'tracked_outstanding_bytes': 0}},
              'live_allocations': 0,
              'tracked_outstanding_bytes': 0,
              'bitmap': '0f',
              'sources': ['fixtures/primitive_slices/coverage.sts'],
              'source_hashes': {'fixtures/primitive_slices/coverage.sts': 'b2fd1ba74357786039b30da03c833c488bb8ad2c219fa2517edcfcec7a8f0105'}},
 'count_order': {'pairs': [[0, 4],
                           [0, 5],
                           [0, 8],
                           [0, 9],
                           [0, 10],
                           [0, 11],
                           [0, 12],
                           [0, 13],
                           [0, 14],
                           [0, 15],
                           [0, 16],
                           [0, 17],
                           [0, 18]],
                 'hits': [[0, 4],
                          [0, 5],
                          [0, 8],
                          [0, 9],
                          [0, 10],
                          [0, 11],
                          [0, 12],
                          [0, 13],
                          [0, 14],
                          [0, 15],
                          [0, 16],
                          [0, 17],
                          [0, 18]],
                 'assertions': 3,
                 'tests': 1,
                 'event_bytes': [16, 6],
                 'allocations': [],
                 'trace': [12, 1, 12],
                 'decl_id': 2,
                 'owner': 'exercise',
                 'static_test_id': None,
                 'sites': [{'id': 0,
                            'file_id': 0,
                            'line': 8,
                            'col': 20,
                            'end_line': 8,
                            'end_col': 55,
                            'kind': 4,
                            'decl_id': 2,
                            'owner': 'exercise',
                            'static_test_id': None,
                            'slice_element_kind': 5,
                            'slice_abi_stride': 4,
                            'slice_count_arity': 2,
                            'binding': 's',
                            'binding_origin_site': 0},
                           {'id': 1,
                            'file_id': 0,
                            'line': 10,
                            'col': 5,
                            'end_line': 10,
                            'end_col': 11,
                            'kind': 5,
                            'decl_id': 2,
                            'owner': 'exercise',
                            'static_test_id': None,
                            'slice_element_kind': 0,
                            'slice_abi_stride': 0,
                            'slice_count_arity': 0,
                            'binding': 's',
                            'binding_origin_site': 0},
                           {'id': 2,
                            'file_id': 0,
                            'line': 12,
                            'col': 20,
                            'end_line': 12,
                            'end_col': 43,
                            'kind': 4,
                            'decl_id': 2,
                            'owner': 'exercise',
                            'static_test_id': None,
                            'slice_element_kind': 3,
                            'slice_abi_stride': 2,
                            'slice_count_arity': 1,
                            'binding': 't',
                            'binding_origin_site': 2},
                           {'id': 3,
                            'file_id': 0,
                            'line': 14,
                            'col': 5,
                            'end_line': 14,
                            'end_col': 11,
                            'kind': 5,
                            'decl_id': 2,
                            'owner': 'exercise',
                            'static_test_id': None,
                            'slice_element_kind': 0,
                            'slice_abi_stride': 0,
                            'slice_count_arity': 0,
                            'binding': 't',
                            'binding_origin_site': 2},
                           {'id': 4,
                            'file_id': 0,
                            'line': 16,
                            'col': 20,
                            'end_line': 16,
                            'end_col': 55,
                            'kind': 4,
                            'decl_id': 2,
                            'owner': 'exercise',
                            'static_test_id': None,
                            'slice_element_kind': 5,
                            'slice_abi_stride': 4,
                            'slice_count_arity': 2,
                            'binding': 'z',
                            'binding_origin_site': 4},
                           {'id': 5,
                            'file_id': 0,
                            'line': 18,
                            'col': 5,
                            'end_line': 18,
                            'end_col': 11,
                            'kind': 5,
                            'decl_id': 2,
                            'owner': 'exercise',
                            'static_test_id': None,
                            'slice_element_kind': 0,
                            'slice_abi_stride': 0,
                            'slice_count_arity': 0,
                            'binding': 'z',
                            'binding_origin_site': 4}],
                 'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                        'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                              'tracked_outstanding_bytes': 0}},
                 'live_allocations': 0,
                 'tracked_outstanding_bytes': 0,
                 'bitmap': 'ff1f',
                 'sources': ['fixtures/primitive_slices/count_order.sts'],
                 'source_hashes': {'fixtures/primitive_slices/count_order.sts': 'a366cb8ecb4fb60f73c0b77e98b327351207c2f3249ce9c6e04d3e9cd0fbea2e'}},
 'same_line': {'pairs': [[0, 3], [0, 4], [0, 5], [0, 6], [0, 7]],
               'hits': [[0, 3], [0, 4], [0, 5], [0, 6], [0, 7]],
               'assertions': 1,
               'tests': 1,
               'event_bytes': [8, 12],
               'allocations': [],
               'trace': 9,
               'decl_id': 0,
               'owner': 'exercise',
               'static_test_id': None,
               'sites': [{'id': 0,
                          'file_id': 0,
                          'line': 3,
                          'col': 20,
                          'end_line': 3,
                          'end_col': 37,
                          'kind': 4,
                          'decl_id': 0,
                          'owner': 'exercise',
                          'static_test_id': None,
                          'slice_element_kind': 5,
                          'slice_abi_stride': 4,
                          'slice_count_arity': 2,
                          'binding': 'a',
                          'binding_origin_site': 0},
                         {'id': 1,
                          'file_id': 0,
                          'line': 3,
                          'col': 55,
                          'end_line': 3,
                          'end_col': 72,
                          'kind': 4,
                          'decl_id': 0,
                          'owner': 'exercise',
                          'static_test_id': None,
                          'slice_element_kind': 5,
                          'slice_abi_stride': 4,
                          'slice_count_arity': 2,
                          'binding': 'b',
                          'binding_origin_site': 1},
                         {'id': 2,
                          'file_id': 0,
                          'line': 6,
                          'col': 5,
                          'end_line': 6,
                          'end_col': 11,
                          'kind': 5,
                          'decl_id': 0,
                          'owner': 'exercise',
                          'static_test_id': None,
                          'slice_element_kind': 0,
                          'slice_abi_stride': 0,
                          'slice_count_arity': 0,
                          'binding': 'b',
                          'binding_origin_site': 1},
                         {'id': 3,
                          'file_id': 0,
                          'line': 6,
                          'col': 14,
                          'end_line': 6,
                          'end_col': 20,
                          'kind': 5,
                          'decl_id': 0,
                          'owner': 'exercise',
                          'static_test_id': None,
                          'slice_element_kind': 0,
                          'slice_abi_stride': 0,
                          'slice_count_arity': 0,
                          'binding': 'a',
                          'binding_origin_site': 0}],
               'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                      'generated_primitive_slice_backing': {'live_allocations': 0,
                                                                            'tracked_outstanding_bytes': 0}},
               'live_allocations': 0,
               'tracked_outstanding_bytes': 0,
               'bitmap': '1f',
               'sources': ['fixtures/primitive_slices/same_line.sts'],
               'source_hashes': {'fixtures/primitive_slices/same_line.sts': '0b053cc9ed36792cabcaabae51e975b73edc3d8b61e4d9997fcf404344e1c6fb'}},
 'imports': {'pairs': [[0, 4], [0, 5], [1, 4], [1, 5]],
             'hits': [[0, 4], [0, 5], [1, 4], [1, 5]],
             'assertions': 3,
             'tests': 2,
             'event_bytes': [8, 12, 12],
             'allocations': [{'generation': 1,
                              'domain': 2,
                              'bytes': 8,
                              'allocation_site': 0,
                              'resize_site': 4294967295,
                              'test': 0},
                             {'generation': 2,
                              'domain': 2,
                              'bytes': 12,
                              'allocation_site': 1,
                              'resize_site': 4294967295,
                              'test': 0},
                             {'generation': 3,
                              'domain': 2,
                              'bytes': 12,
                              'allocation_site': 1,
                              'resize_site': 4294967295,
                              'test': 1}],
             'trace': None,
             'decl_id': 1,
             'owner': 'local',
             'static_test_id': None,
             'sites': [{'id': 0,
                        'file_id': 0,
                        'line': 4,
                        'col': 20,
                        'end_line': 4,
                        'end_col': 37,
                        'kind': 4,
                        'decl_id': 1,
                        'owner': 'local',
                        'static_test_id': None,
                        'slice_element_kind': 5,
                        'slice_abi_stride': 4,
                        'slice_count_arity': 2,
                        'binding': 's',
                        'binding_origin_site': 0},
                       {'id': 1,
                        'file_id': 1,
                        'line': 4,
                        'col': 20,
                        'end_line': 4,
                        'end_col': 37,
                        'kind': 4,
                        'decl_id': 3,
                        'owner': 'keep',
                        'static_test_id': None,
                        'slice_element_kind': 5,
                        'slice_abi_stride': 4,
                        'slice_count_arity': 2,
                        'binding': 's',
                        'binding_origin_site': 1}],
             'allocation_domains': {'explicit_user_heap': {'live_allocations': 0, 'tracked_outstanding_bytes': 0},
                                    'generated_primitive_slice_backing': {'live_allocations': 3,
                                                                          'tracked_outstanding_bytes': 32}},
             'live_allocations': 3,
             'tracked_outstanding_bytes': 32,
             'bitmap': '0f',
             'sources': ['fixtures/primitive_slices/imports/root.sts', 'fixtures/primitive_slices/imports/util/slice.sts'],
             'source_hashes': {'fixtures/primitive_slices/imports/root.sts': '27a74e34315887c0ccc30d5c0d9dccda0ea1cd560353f917bb303e90f9b24c52',
                               'fixtures/primitive_slices/imports/util/slice.sts': 'e3d0a0b59b9db1a141ba48be9b4388892229d39a6c4fdb4adc00183fdd146f4c'}}}

REJECTIONS = {'descriptor_copy-called': {'id': 'descriptor_copy', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'descriptor_copy-uncalled': {'id': 'descriptor_copy',
                              'context': 'uncalled',
                              'diagnostic_owner': 'observer',
                              'native_executions': 0},
 'descriptor_copy-dead': {'id': 'descriptor_copy', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'descriptor_assignment-called': {'id': 'descriptor_assignment',
                                  'context': 'called',
                                  'diagnostic_owner': 'observer',
                                  'native_executions': 0},
 'descriptor_assignment-uncalled': {'id': 'descriptor_assignment',
                                    'context': 'uncalled',
                                    'diagnostic_owner': 'observer',
                                    'native_executions': 0},
 'descriptor_assignment-dead': {'id': 'descriptor_assignment',
                                'context': 'dead',
                                'diagnostic_owner': 'observer',
                                'native_executions': 0},
 'uninitialized-called': {'id': 'uninitialized', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'uninitialized-uncalled': {'id': 'uninitialized', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'uninitialized-dead': {'id': 'uninitialized', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'nil_declaration-called': {'id': 'nil_declaration', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'nil_declaration-uncalled': {'id': 'nil_declaration',
                              'context': 'uncalled',
                              'diagnostic_owner': 'observer',
                              'native_executions': 0},
 'nil_declaration-dead': {'id': 'nil_declaration', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'inferred_declaration-called': {'id': 'inferred_declaration',
                                 'context': 'called',
                                 'diagnostic_owner': 'observer',
                                 'native_executions': 0},
 'inferred_declaration-uncalled': {'id': 'inferred_declaration',
                                   'context': 'uncalled',
                                   'diagnostic_owner': 'observer',
                                   'native_executions': 0},
 'inferred_declaration-dead': {'id': 'inferred_declaration',
                               'context': 'dead',
                               'diagnostic_owner': 'observer',
                               'native_executions': 0},
 'discarded_declaration-called': {'id': 'discarded_declaration',
                                  'context': 'called',
                                  'diagnostic_owner': 'observer',
                                  'native_executions': 0},
 'discarded_declaration-uncalled': {'id': 'discarded_declaration',
                                    'context': 'uncalled',
                                    'diagnostic_owner': 'observer',
                                    'native_executions': 0},
 'discarded_declaration-dead': {'id': 'discarded_declaration',
                                'context': 'dead',
                                'diagnostic_owner': 'observer',
                                'native_executions': 0},
 'stack_declaration-called': {'id': 'stack_declaration',
                              'context': 'called',
                              'diagnostic_owner': 'observer',
                              'native_executions': 0},
 'stack_declaration-uncalled': {'id': 'stack_declaration',
                                'context': 'uncalled',
                                'diagnostic_owner': 'observer',
                                'native_executions': 0},
 'stack_declaration-dead': {'id': 'stack_declaration', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'qualified_declaration-called': {'id': 'qualified_declaration',
                                  'context': 'called',
                                  'diagnostic_owner': 'observer',
                                  'native_executions': 0},
 'qualified_declaration-uncalled': {'id': 'qualified_declaration',
                                    'context': 'uncalled',
                                    'diagnostic_owner': 'observer',
                                    'native_executions': 0},
 'qualified_declaration-dead': {'id': 'qualified_declaration',
                                'context': 'dead',
                                'diagnostic_owner': 'observer',
                                'native_executions': 0},
 'inline_make-called': {'id': 'inline_make', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'inline_make-uncalled': {'id': 'inline_make', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'inline_make-dead': {'id': 'inline_make', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'nested_constructor-called': {'id': 'nested_constructor',
                               'context': 'called',
                               'diagnostic_owner': 'observer',
                               'native_executions': 0},
 'nested_constructor-uncalled': {'id': 'nested_constructor',
                                 'context': 'uncalled',
                                 'diagnostic_owner': 'observer',
                                 'native_executions': 0},
 'nested_constructor-dead': {'id': 'nested_constructor',
                             'context': 'dead',
                             'diagnostic_owner': 'observer',
                             'native_executions': 0},
 'for_initializer-called': {'id': 'for_initializer', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'for_initializer-uncalled': {'id': 'for_initializer',
                              'context': 'uncalled',
                              'diagnostic_owner': 'observer',
                              'native_executions': 0},
 'for_initializer-dead': {'id': 'for_initializer', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'fat_arrow_constructor-called': {'id': 'fat_arrow_constructor',
                                  'context': 'called',
                                  'diagnostic_owner': 'observer',
                                  'native_executions': 0},
 'fat_arrow_constructor-uncalled': {'id': 'fat_arrow_constructor',
                                    'context': 'uncalled',
                                    'diagnostic_owner': 'observer',
                                    'native_executions': 0},
 'fat_arrow_constructor-dead': {'id': 'fat_arrow_constructor',
                                'context': 'dead',
                                'diagnostic_owner': 'observer',
                                'native_executions': 0},
 'deferred_constructor-called': {'id': 'deferred_constructor',
                                 'context': 'called',
                                 'diagnostic_owner': 'observer',
                                 'native_executions': 0},
 'deferred_constructor-uncalled': {'id': 'deferred_constructor',
                                   'context': 'uncalled',
                                   'diagnostic_owner': 'observer',
                                   'native_executions': 0},
 'deferred_constructor-dead': {'id': 'deferred_constructor',
                               'context': 'dead',
                               'diagnostic_owner': 'observer',
                               'native_executions': 0},
 'descriptor_argument-called': {'id': 'descriptor_argument',
                                'context': 'called',
                                'diagnostic_owner': 'observer',
                                'native_executions': 0},
 'descriptor_argument-uncalled': {'id': 'descriptor_argument',
                                  'context': 'uncalled',
                                  'diagnostic_owner': 'observer',
                                  'native_executions': 0},
 'descriptor_argument-dead': {'id': 'descriptor_argument',
                              'context': 'dead',
                              'diagnostic_owner': 'observer',
                              'native_executions': 0},
 'descriptor_result-called': {'id': 'descriptor_result',
                              'context': 'called',
                              'diagnostic_owner': 'observer',
                              'native_executions': 0},
 'descriptor_result-uncalled': {'id': 'descriptor_result',
                                'context': 'uncalled',
                                'diagnostic_owner': 'observer',
                                'native_executions': 0},
 'descriptor_result-dead': {'id': 'descriptor_result', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'descriptor_address-called': {'id': 'descriptor_address',
                               'context': 'called',
                               'diagnostic_owner': 'observer',
                               'native_executions': 0},
 'descriptor_address-uncalled': {'id': 'descriptor_address',
                                 'context': 'uncalled',
                                 'diagnostic_owner': 'observer',
                                 'native_executions': 0},
 'descriptor_address-dead': {'id': 'descriptor_address',
                             'context': 'dead',
                             'diagnostic_owner': 'observer',
                             'native_executions': 0},
 'element_address-called': {'id': 'element_address', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'element_address-uncalled': {'id': 'element_address',
                              'context': 'uncalled',
                              'diagnostic_owner': 'observer',
                              'native_executions': 0},
 'element_address-dead': {'id': 'element_address', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'nested_element_address-called': {'id': 'nested_element_address',
                                   'context': 'called',
                                   'diagnostic_owner': 'observer',
                                   'native_executions': 0},
 'nested_element_address-uncalled': {'id': 'nested_element_address',
                                     'context': 'uncalled',
                                     'diagnostic_owner': 'observer',
                                     'native_executions': 0},
 'nested_element_address-dead': {'id': 'nested_element_address',
                                 'context': 'dead',
                                 'diagnostic_owner': 'observer',
                                 'native_executions': 0},
 'whole_expression-called': {'id': 'whole_expression',
                             'context': 'called',
                             'diagnostic_owner': 'observer',
                             'native_executions': 0},
 'whole_expression-uncalled': {'id': 'whole_expression',
                               'context': 'uncalled',
                               'diagnostic_owner': 'observer',
                               'native_executions': 0},
 'whole_expression-dead': {'id': 'whole_expression', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'whole_assertion-called': {'id': 'whole_assertion', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'whole_assertion-uncalled': {'id': 'whole_assertion',
                              'context': 'uncalled',
                              'diagnostic_owner': 'observer',
                              'native_executions': 0},
 'whole_assertion-dead': {'id': 'whole_assertion', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'whole_comparison-called': {'id': 'whole_comparison',
                             'context': 'called',
                             'diagnostic_owner': 'observer',
                             'native_executions': 0},
 'whole_comparison-uncalled': {'id': 'whole_comparison',
                               'context': 'uncalled',
                               'diagnostic_owner': 'observer',
                               'native_executions': 0},
 'whole_comparison-dead': {'id': 'whole_comparison', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'view-called': {'id': 'view', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'view-uncalled': {'id': 'view', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'view-dead': {'id': 'view', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'append-called': {'id': 'append', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'append-uncalled': {'id': 'append', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'append-dead': {'id': 'append', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'copy-called': {'id': 'copy', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'copy-uncalled': {'id': 'copy', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'copy-dead': {'id': 'copy', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'compound_element-called': {'id': 'compound_element',
                             'context': 'called',
                             'diagnostic_owner': 'observer',
                             'native_executions': 0},
 'compound_element-uncalled': {'id': 'compound_element',
                               'context': 'uncalled',
                               'diagnostic_owner': 'observer',
                               'native_executions': 0},
 'compound_element-dead': {'id': 'compound_element', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'postfix_element-called': {'id': 'postfix_element', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'postfix_element-uncalled': {'id': 'postfix_element',
                              'context': 'uncalled',
                              'diagnostic_owner': 'observer',
                              'native_executions': 0},
 'postfix_element-dead': {'id': 'postfix_element', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'double_release-called': {'id': 'double_release', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'double_release-uncalled': {'id': 'double_release',
                             'context': 'uncalled',
                             'diagnostic_owner': 'observer',
                             'native_executions': 0},
 'double_release-dead': {'id': 'double_release', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'release_then_use-called': {'id': 'release_then_use',
                             'context': 'called',
                             'diagnostic_owner': 'observer',
                             'native_executions': 0},
 'release_then_use-uncalled': {'id': 'release_then_use',
                               'context': 'uncalled',
                               'diagnostic_owner': 'observer',
                               'native_executions': 0},
 'release_then_use-dead': {'id': 'release_then_use', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'release_then_nested_use-called': {'id': 'release_then_nested_use',
                                    'context': 'called',
                                    'diagnostic_owner': 'observer',
                                    'native_executions': 0},
 'release_then_nested_use-uncalled': {'id': 'release_then_nested_use',
                                      'context': 'uncalled',
                                      'diagnostic_owner': 'observer',
                                      'native_executions': 0},
 'release_then_nested_use-dead': {'id': 'release_then_nested_use',
                                  'context': 'dead',
                                  'diagnostic_owner': 'observer',
                                  'native_executions': 0},
 'conditional_release-called': {'id': 'conditional_release',
                                'context': 'called',
                                'diagnostic_owner': 'observer',
                                'native_executions': 0},
 'conditional_release-uncalled': {'id': 'conditional_release',
                                  'context': 'uncalled',
                                  'diagnostic_owner': 'observer',
                                  'native_executions': 0},
 'conditional_release-dead': {'id': 'conditional_release',
                              'context': 'dead',
                              'diagnostic_owner': 'observer',
                              'native_executions': 0},
 'nested_release-called': {'id': 'nested_release', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'nested_release-uncalled': {'id': 'nested_release',
                             'context': 'uncalled',
                             'diagnostic_owner': 'observer',
                             'native_executions': 0},
 'nested_release-dead': {'id': 'nested_release', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'loop_release_outer-called': {'id': 'loop_release_outer',
                               'context': 'called',
                               'diagnostic_owner': 'observer',
                               'native_executions': 0},
 'loop_release_outer-uncalled': {'id': 'loop_release_outer',
                                 'context': 'uncalled',
                                 'diagnostic_owner': 'observer',
                                 'native_executions': 0},
 'loop_release_outer-dead': {'id': 'loop_release_outer',
                             'context': 'dead',
                             'diagnostic_owner': 'observer',
                             'native_executions': 0},
 'release_expression-called': {'id': 'release_expression',
                               'context': 'called',
                               'diagnostic_owner': 'observer',
                               'native_executions': 0},
 'release_expression-uncalled': {'id': 'release_expression',
                                 'context': 'uncalled',
                                 'diagnostic_owner': 'observer',
                                 'native_executions': 0},
 'release_expression-dead': {'id': 'release_expression',
                             'context': 'dead',
                             'diagnostic_owner': 'observer',
                             'native_executions': 0},
 'release_plus_defer-called': {'id': 'release_plus_defer',
                               'context': 'called',
                               'diagnostic_owner': 'observer',
                               'native_executions': 0},
 'release_plus_defer-uncalled': {'id': 'release_plus_defer',
                                 'context': 'uncalled',
                                 'diagnostic_owner': 'observer',
                                 'native_executions': 0},
 'release_plus_defer-dead': {'id': 'release_plus_defer',
                             'context': 'dead',
                             'diagnostic_owner': 'observer',
                             'native_executions': 0},
 'delayed_defer-called': {'id': 'delayed_defer', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'delayed_defer-uncalled': {'id': 'delayed_defer', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'delayed_defer-dead': {'id': 'delayed_defer', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'defer_single_statement-called': {'id': 'defer_single_statement',
                                   'context': 'called',
                                   'diagnostic_owner': 'observer',
                                   'native_executions': 0},
 'defer_single_statement-uncalled': {'id': 'defer_single_statement',
                                     'context': 'uncalled',
                                     'diagnostic_owner': 'observer',
                                     'native_executions': 0},
 'defer_single_statement-dead': {'id': 'defer_single_statement',
                                 'context': 'dead',
                                 'diagnostic_owner': 'observer',
                                 'native_executions': 0},
 'defer_extra_statement-called': {'id': 'defer_extra_statement',
                                  'context': 'called',
                                  'diagnostic_owner': 'observer',
                                  'native_executions': 0},
 'defer_extra_statement-uncalled': {'id': 'defer_extra_statement',
                                    'context': 'uncalled',
                                    'diagnostic_owner': 'observer',
                                    'native_executions': 0},
 'defer_extra_statement-dead': {'id': 'defer_extra_statement',
                                'context': 'dead',
                                'diagnostic_owner': 'observer',
                                'native_executions': 0},
 'deferred_read-called': {'id': 'deferred_read', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'deferred_read-uncalled': {'id': 'deferred_read', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'deferred_read-dead': {'id': 'deferred_read', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'direct_with_deferred_read-called': {'id': 'direct_with_deferred_read',
                                      'context': 'called',
                                      'diagnostic_owner': 'observer',
                                      'native_executions': 0},
 'direct_with_deferred_read-uncalled': {'id': 'direct_with_deferred_read',
                                        'context': 'uncalled',
                                        'diagnostic_owner': 'observer',
                                        'native_executions': 0},
 'direct_with_deferred_read-dead': {'id': 'direct_with_deferred_read',
                                    'context': 'dead',
                                    'diagnostic_owner': 'observer',
                                    'native_executions': 0},
 'wrong_element_identity-called': {'id': 'wrong_element_identity',
                                   'context': 'called',
                                   'diagnostic_owner': 'ordinary-or-observer',
                                   'native_executions': 0},
 'wrong_element_identity-uncalled': {'id': 'wrong_element_identity',
                                     'context': 'uncalled',
                                     'diagnostic_owner': 'ordinary-or-observer',
                                     'native_executions': 0},
 'wrong_element_identity-dead': {'id': 'wrong_element_identity',
                                 'context': 'dead',
                                 'diagnostic_owner': 'ordinary-or-observer',
                                 'native_executions': 0},
 'float_count-called': {'id': 'float_count',
                        'context': 'called',
                        'diagnostic_owner': 'ordinary-or-observer',
                        'native_executions': 0},
 'float_count-uncalled': {'id': 'float_count',
                          'context': 'uncalled',
                          'diagnostic_owner': 'ordinary-or-observer',
                          'native_executions': 0},
 'float_count-dead': {'id': 'float_count', 'context': 'dead', 'diagnostic_owner': 'ordinary-or-observer', 'native_executions': 0},
 'pointer_count-called': {'id': 'pointer_count',
                          'context': 'called',
                          'diagnostic_owner': 'ordinary-or-observer',
                          'native_executions': 0},
 'pointer_count-uncalled': {'id': 'pointer_count',
                            'context': 'uncalled',
                            'diagnostic_owner': 'ordinary-or-observer',
                            'native_executions': 0},
 'pointer_count-dead': {'id': 'pointer_count',
                        'context': 'dead',
                        'diagnostic_owner': 'ordinary-or-observer',
                        'native_executions': 0},
 'nested_element_type-called': {'id': 'nested_element_type',
                                'context': 'called',
                                'diagnostic_owner': 'ordinary-or-observer',
                                'native_executions': 0},
 'nested_element_type-uncalled': {'id': 'nested_element_type',
                                  'context': 'uncalled',
                                  'diagnostic_owner': 'ordinary-or-observer',
                                  'native_executions': 0},
 'nested_element_type-dead': {'id': 'nested_element_type',
                              'context': 'dead',
                              'diagnostic_owner': 'ordinary-or-observer',
                              'native_executions': 0},
 'pointer_element_type-called': {'id': 'pointer_element_type',
                                 'context': 'called',
                                 'diagnostic_owner': 'ordinary-or-observer',
                                 'native_executions': 0},
 'pointer_element_type-uncalled': {'id': 'pointer_element_type',
                                   'context': 'uncalled',
                                   'diagnostic_owner': 'ordinary-or-observer',
                                   'native_executions': 0},
 'pointer_element_type-dead': {'id': 'pointer_element_type',
                               'context': 'dead',
                               'diagnostic_owner': 'ordinary-or-observer',
                               'native_executions': 0},
 'synthetic_match_binding-called': {'id': 'synthetic_match_binding',
                                    'context': 'called',
                                    'diagnostic_owner': 'observer',
                                    'native_executions': 0},
 'synthetic_match_binding-uncalled': {'id': 'synthetic_match_binding',
                                      'context': 'uncalled',
                                      'diagnostic_owner': 'observer',
                                      'native_executions': 0},
 'synthetic_match_binding-dead': {'id': 'synthetic_match_binding',
                                  'context': 'dead',
                                  'diagnostic_owner': 'observer',
                                  'native_executions': 0},
 'element_alias-called': {'id': 'element_alias', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'element_alias-uncalled': {'id': 'element_alias', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'element_alias-dead': {'id': 'element_alias', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'composite_element-called': {'id': 'composite_element',
                              'context': 'called',
                              'diagnostic_owner': 'ordinary-or-observer',
                              'native_executions': 0},
 'composite_element-uncalled': {'id': 'composite_element',
                                'context': 'uncalled',
                                'diagnostic_owner': 'ordinary-or-observer',
                                'native_executions': 0},
 'composite_element-dead': {'id': 'composite_element',
                            'context': 'dead',
                            'diagnostic_owner': 'ordinary-or-observer',
                            'native_executions': 0},
 'descriptor_cast-called': {'id': 'descriptor_cast',
                            'context': 'called',
                            'diagnostic_owner': 'ordinary-or-observer',
                            'native_executions': 0},
 'descriptor_cast-uncalled': {'id': 'descriptor_cast',
                              'context': 'uncalled',
                              'diagnostic_owner': 'ordinary-or-observer',
                              'native_executions': 0},
 'descriptor_cast-dead': {'id': 'descriptor_cast',
                          'context': 'dead',
                          'diagnostic_owner': 'ordinary-or-observer',
                          'native_executions': 0},
 'member_query-called': {'id': 'member_query', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'member_query-uncalled': {'id': 'member_query', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'member_query-dead': {'id': 'member_query', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'whole_print-called': {'id': 'whole_print', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'whole_print-uncalled': {'id': 'whole_print', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'whole_print-dead': {'id': 'whole_print', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'foreach-called': {'id': 'foreach', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'foreach-uncalled': {'id': 'foreach', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'foreach-dead': {'id': 'foreach', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'unsafe_slice_use-called': {'id': 'unsafe_slice_use',
                             'context': 'called',
                             'diagnostic_owner': 'observer',
                             'native_executions': 0},
 'unsafe_slice_use-uncalled': {'id': 'unsafe_slice_use',
                               'context': 'uncalled',
                               'diagnostic_owner': 'observer',
                               'native_executions': 0},
 'unsafe_slice_use-dead': {'id': 'unsafe_slice_use', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'unchecked_slice_use-called': {'id': 'unchecked_slice_use',
                                'context': 'called',
                                'diagnostic_owner': 'observer',
                                'native_executions': 0},
 'unchecked_slice_use-uncalled': {'id': 'unchecked_slice_use',
                                  'context': 'uncalled',
                                  'diagnostic_owner': 'observer',
                                  'native_executions': 0},
 'unchecked_slice_use-dead': {'id': 'unchecked_slice_use',
                              'context': 'dead',
                              'diagnostic_owner': 'observer',
                              'native_executions': 0},
 'slice_array-called': {'id': 'slice_array', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'slice_array-uncalled': {'id': 'slice_array', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'slice_array-dead': {'id': 'slice_array', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'generic_constructor-called': {'id': 'generic_constructor',
                                'context': 'called',
                                'diagnostic_owner': 'observer',
                                'native_executions': 0},
 'generic_constructor-uncalled': {'id': 'generic_constructor',
                                  'context': 'uncalled',
                                  'diagnostic_owner': 'observer',
                                  'native_executions': 0},
 'generic_constructor-dead': {'id': 'generic_constructor',
                              'context': 'dead',
                              'diagnostic_owner': 'observer',
                              'native_executions': 0},
 'captured_use-called': {'id': 'captured_use', 'context': 'called', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'captured_use-uncalled': {'id': 'captured_use', 'context': 'uncalled', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'captured_use-dead': {'id': 'captured_use', 'context': 'dead', 'diagnostic_owner': 'observer', 'native_executions': 0},
 'conditional_descriptor_query-called': {'id': 'conditional_descriptor_query',
                                         'context': 'called',
                                         'diagnostic_owner': 'observer',
                                         'native_executions': 0},
 'conditional_descriptor_query-uncalled': {'id': 'conditional_descriptor_query',
                                           'context': 'uncalled',
                                           'diagnostic_owner': 'observer',
                                           'native_executions': 0},
 'conditional_descriptor_query-dead': {'id': 'conditional_descriptor_query',
                                       'context': 'dead',
                                       'diagnostic_owner': 'observer',
                                       'native_executions': 0},
 'assignment_descriptor_query-called': {'id': 'assignment_descriptor_query',
                                        'context': 'called',
                                        'diagnostic_owner': 'observer',
                                        'native_executions': 0},
 'assignment_descriptor_query-uncalled': {'id': 'assignment_descriptor_query',
                                          'context': 'uncalled',
                                          'diagnostic_owner': 'observer',
                                          'native_executions': 0},
 'assignment_descriptor_query-dead': {'id': 'assignment_descriptor_query',
                                      'context': 'dead',
                                      'diagnostic_owner': 'observer',
                                      'native_executions': 0},
 'global_slice': {'id': 'global_slice', 'context': None, 'diagnostic_owner': 'observer', 'native_executions': 0},
 'query_raw_pointer': {'id': 'query_raw_pointer', 'context': None, 'diagnostic_owner': 'observer', 'native_executions': 0},
 'query_descriptor_call': {'id': 'query_descriptor_call',
                           'context': None,
                           'diagnostic_owner': 'observer',
                           'native_executions': 0}}

COUNT_FAILURE_IDS = ('len_i8_neg',
 'cap_i8_neg',
 'len_i16_neg',
 'cap_i16_neg',
 'len_i32_neg',
 'cap_i32_neg',
 'len_i64_neg',
 'cap_i64_neg',
 'len_i64_over',
 'cap_i64_over',
 'len_u32_over',
 'cap_u32_over',
 'len_u32_max',
 'cap_u32_max',
 'len_u64_wrap',
 'cap_u64_wrap',
 'len_u64_wrap1',
 'cap_u64_wrap1',
 'len_u64_max',
 'cap_u64_max',
 'len_gt_cap',
 'invalid_order')

DYNAMIC_EVENTS = {'primitive_i8': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 5, 'site': 0, 'test': 0},
                  {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'primitive_u8': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 5, 'site': 0, 'test': 0},
                  {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'primitive_i16': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 10, 'site': 0, 'test': 0},
                   {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'primitive_u16': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 10, 'site': 0, 'test': 0},
                   {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'primitive_i32': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 20, 'site': 0, 'test': 0},
                   {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'primitive_u32': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 20, 'site': 0, 'test': 0},
                   {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'primitive_i64': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 40, 'site': 0, 'test': 0},
                   {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'primitive_u64': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 40, 'site': 0, 'test': 0},
                   {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'primitive_bool': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 5, 'site': 0, 'test': 0},
                    {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'primitive_f32': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 20, 'site': 0, 'test': 0},
                   {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'primitive_f64': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 40, 'site': 0, 'test': 0},
                   {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'count_i8': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
              {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'count_u8': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
              {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'count_i16': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
               {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'count_u16': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
               {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'count_i32': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
               {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'count_u32': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
               {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'count_i64': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
               {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'count_u64': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
               {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'count_bool': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
                {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'count_unsigned_top_bit': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 256, 'site': 0, 'test': 0},
                            {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'retained_i32': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 20, 'site': 0, 'test': 0}],
 'retained_empty': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 12, 'site': 0, 'test': 0}],
 'omitted_i16': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 6, 'site': 0, 'test': 0}],
 'zero_omitted': [],
 'zero_explicit': [],
 'mixed_both': [{'operation': 'allocate', 'generation': 1, 'domain': 1, 'bytes': 7, 'site': 0, 'test': 0},
                {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 20, 'site': 1, 'test': 0}],
 'mixed_explicit_only': [{'operation': 'allocate', 'generation': 1, 'domain': 1, 'bytes': 7, 'site': 0, 'test': 0},
                         {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 20, 'site': 1, 'test': 0},
                         {'operation': 'release', 'generation': 2, 'domain': 2, 'site': 2}],
 'mixed_slice_only': [{'operation': 'allocate', 'generation': 1, 'domain': 1, 'bytes': 7, 'site': 0, 'test': 0},
                      {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 20, 'site': 1, 'test': 0},
                      {'operation': 'release', 'generation': 1, 'domain': 1, 'site': 2}],
 'schedule_direct': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 8, 'site': 0, 'test': 0},
                     {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'schedule_defer_fallthrough': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 8, 'site': 0, 'test': 0},
                                {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'schedule_defer_return': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 8, 'site': 0, 'test': 0},
                           {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'schedule_defer_break': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
                          {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'schedule_defer_nested': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 8, 'site': 0, 'test': 0},
                           {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 6, 'site': 2, 'test': 0},
                           {'operation': 'release', 'generation': 2, 'domain': 2, 'site': 3},
                           {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'schedule_shadow_defer': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 20, 'site': 0, 'test': 0},
                           {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 6, 'site': 2, 'test': 0},
                           {'operation': 'release', 'generation': 2, 'domain': 2, 'site': 3},
                           {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'schedule_defer_continue': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
                             {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1},
                             {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
                             {'operation': 'release', 'generation': 2, 'domain': 2, 'site': 1},
                             {'operation': 'allocate', 'generation': 3, 'domain': 2, 'bytes': 4, 'site': 0, 'test': 0},
                             {'operation': 'release', 'generation': 3, 'domain': 2, 'site': 1}],
 'schedule_defer_nested_exits': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 2, 'site': 0, 'test': 0},
                                 {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 6, 'site': 2, 'test': 0},
                                 {'operation': 'allocate', 'generation': 3, 'domain': 2, 'bytes': 16, 'site': 4, 'test': 0},
                                 {'operation': 'release', 'generation': 3, 'domain': 2, 'site': 5},
                                 {'operation': 'release', 'generation': 2, 'domain': 2, 'site': 3},
                                 {'operation': 'allocate', 'generation': 4, 'domain': 2, 'bytes': 6, 'site': 2, 'test': 0},
                                 {'operation': 'allocate', 'generation': 5, 'domain': 2, 'bytes': 16, 'site': 4, 'test': 0},
                                 {'operation': 'release', 'generation': 5, 'domain': 2, 'site': 5},
                                 {'operation': 'release', 'generation': 4, 'domain': 2, 'site': 3},
                                 {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'schedule_direct_bypass_return': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 20, 'site': 0, 'test': 0}],
 'schedule_direct_bypass_break': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 20, 'site': 0, 'test': 0}],
 'schedule_direct_bypass_continue': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 20, 'site': 0, 'test': 0},
                                     {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 20, 'site': 0, 'test': 0}],
 'schedule_shadow_siblings': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 8, 'site': 0, 'test': 0},
                              {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1},
                              {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 6, 'site': 2, 'test': 0},
                              {'operation': 'release', 'generation': 2, 'domain': 2, 'site': 3}],
 'nested_scalar': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 20, 'site': 0, 'test': 0},
                   {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 8, 'site': 2, 'test': 0},
                   {'operation': 'release', 'generation': 2, 'domain': 2, 'site': 3},
                   {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'no_assertions': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 8, 'site': 0, 'test': 0},
                   {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'coverage': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 20, 'site': 0, 'test': 0},
              {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1}],
 'count_order': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 16, 'site': 0, 'test': 0},
                 {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 1},
                 {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 6, 'site': 2, 'test': 0},
                 {'operation': 'release', 'generation': 2, 'domain': 2, 'site': 3}],
 'same_line': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 8, 'site': 0, 'test': 0},
               {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 12, 'site': 1, 'test': 0},
               {'operation': 'release', 'generation': 2, 'domain': 2, 'site': 2},
               {'operation': 'release', 'generation': 1, 'domain': 2, 'site': 3}],
 'imports': [{'operation': 'allocate', 'generation': 1, 'domain': 2, 'bytes': 8, 'site': 0, 'test': 0},
             {'operation': 'allocate', 'generation': 2, 'domain': 2, 'bytes': 12, 'site': 1, 'test': 0},
             {'operation': 'allocate', 'generation': 3, 'domain': 2, 'bytes': 12, 'site': 1, 'test': 1}]}

# Observer v4 promotes exactly these source-identical positional constructors.
# Keep every historical source/hash and ordinary/count oracle; only dispatch moves.
PROMOTED_REJECTIONS = ('inline_make-called', 'inline_make-dead', 'inline_make-uncalled')
for _promoted in PROMOTED_REJECTIONS:
    assert REJECTIONS.pop(_promoted)['id'] == 'inline_make'
for _oracle in ORACLES.values():
    for _site in _oracle['sites']:
        _site['slice_initializer_item_count'] = 0


def modes_for(name):
    assert name in NATIVE_NAMES
    return ('warn', 'max_equal', 'coverage_only', 'combined',
            *(('max_below',) if name in BELOW_NAMES else ()),
            *(('coverage_66', 'coverage_67') if name == 'coverage' else ()))


def mode_oracle(name, mode):
    assert mode in modes_for(name)
    oracle = ORACLES[name]
    maximum = None if mode in ('warn', 'coverage_only') else oracle['tracked_outstanding_bytes'] - (mode == 'max_below')
    minimum = (66 if mode == 'coverage_66' else 67 if mode == 'coverage_67' else 0) if mode in ('coverage_only', 'combined', 'coverage_66', 'coverage_67') else None
    warn = mode in ('warn', 'combined')
    expected = int((maximum is not None and maximum < oracle['tracked_outstanding_bytes']) or
                   (minimum is not None and (not oracle['pairs'] or 100 * len(oracle['hits']) < minimum * len(oracle['pairs']))))
    return dict(maximum=maximum, minimum=minimum, warn=warn, expected=expected,
                enabled=(int(warn or maximum is not None) | (2 if minimum is not None else 0)),
                hits=oracle['hits'] if minimum is not None else [],
                warning=warn and oracle['live_allocations'] > 0)


def flags_for(mode):
    flags = ['--warn-memleak'] if mode['warn'] else []
    if mode['maximum'] is not None: flags += ['--max-leaked-bytes', str(mode['maximum'])]
    if mode['minimum'] is not None: flags += ['--min-test-coverage', str(mode['minimum'])]
    return flags


def expected_cases(levels):
    return {'ordinary_native': [f'{name}-ordinary-O{opt}' for opt in levels for name in NATIVE_NAMES],
            'observed_native': [f'{name}-{mode}-O{opt}' for opt in levels for name in NATIVE_NAMES for mode in modes_for(name)]}


def nonlaunch_inventory(levels):
    return {'compile_only': ['reject-' + name for name in REJECTIONS] +
            [f'{name}-{mode}-ir-O{opt}' for opt in levels for name in NATIVE_NAMES for mode in ('ordinary', 'warn', 'combined')] +
            [f'count-{name}-{mode}-ir-O{opt}' for opt in levels for name in COUNT_FAILURE_IDS for mode in ('ordinary', 'observed')]}


def materialize(session):
    assert set(ORACLES) == set(NATIVE_NAMES)
    assert {str(p.relative_to(FIXTURES)) for p in FIXTURES.rglob('*.sts')} == set(FIXTURE_HASHES)
    for relative, digest in FIXTURE_HASHES.items(): assert sha(FIXTURES / relative) == digest
    for relative, source in GENERATED_SOURCES.items():
        assert relative.startswith('sources/primitive-slices/') and '..' not in Path(relative).parts
        path = session.artifacts / relative
        path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(source.encode('ascii'))
        assert sha(path) == SOURCE_HASHES[relative]
    positive = {}
    for name in NATIVE_NAMES:
        relative = ORACLES[name]['sources'][0]
        positive[name] = (FIXTURES / relative.removeprefix('fixtures/primitive_slices/')) if relative.startswith('fixtures/') else session.artifacts / relative
    session.freeze_generated()
    assert session.generated == {p: h for p, h in SOURCE_HASHES.items() if p.startswith('sources/')}
    (session.artifacts / 'literal-oracles.json').write_text(json.dumps(ORACLES, indent=2) + '\n')
    (session.artifacts / 'dynamic-events.json').write_text(json.dumps(DYNAMIC_EVENTS, indent=2) + '\n')
    session.oracle_hash = sha(session.artifacts / 'literal-oracles.json')
    session.event_hash = sha(session.artifacts / 'dynamic-events.json')
    return positive


def validate_positive_sources(session, sources):
    """Fail before command dispatch if any unsafe or substituted source is selected."""
    assert tuple(sources) == NATIVE_NAMES
    for name, path in sources.items():
        oracle = ORACLES[name]
        relative = oracle['sources'][0]
        expected = FIXTURES / relative.removeprefix('fixtures/primitive_slices/') if relative.startswith('fixtures/') else session.artifacts / relative
        assert path == expected and path.resolve() == expected.absolute() and not path.is_symlink()
        assert sha(path) == oracle['source_hashes'][relative]
    session.verify_inputs()


def assert_output(result, oracle):
    assert result.stderr == b'', result.stderr
    output = result.stdout.decode('ascii')
    assert output.count('PASS\n') == oracle['tests'] and 'FAIL\n' not in output, output
    assert re.search(rf"\n{oracle['assertions']} passed, 0 failed\n\Z", output), output


def run_ordinary(session, sources, levels):
    validate_positive_sources(session, sources)
    for opt in levels:
        for name in NATIVE_NAMES:
            key = f'{name}-ordinary-O{opt}'; entry = sources[name]; oracle = ORACLES[name]
            binary = session.artifacts / (key + '.test')
            pre, post = [session.artifacts / (key + suffix) for suffix in ('.pre.ll', '.post.ll')]
            env = dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post))
            _, compiled = session.command(key + '-compile', [session.compiler, 'test', entry, f'-o={opt}', '-o', binary], env=env)
            assert b'PASS\n' not in compiled.stdout and b'passed, ' not in compiled.stdout
            assert not support.POSITIVE_NONLAUNCH_FAILURES.search(compiled.stdout + compiled.stderr)
            assert binary.is_file() and not binary.is_symlink() and os.access(binary, os.X_OK)
            assert b'__stasha_obs_' not in binary.read_bytes()
            for path in (pre, post): assert path.is_file() and '__stasha_obs_' not in path.read_text()
            digest = sha(binary)
            _, result = session.command(key + '-execute', [binary]); assert_output(result, oracle)
            assert sha(binary) == digest
            session.cases.append(dict(name=key, stage='ordinary_native', status='pass', executions=1,
                binary_sha256=digest, source_sha256=sha(entry), pre_ir_sha256=sha(pre), post_ir_sha256=sha(post),
                assertions_passed=oracle['assertions'], tests=oracle['tests']))


def validate_sites(data, oracle):
    expected = [{k: v for k, v in s.items() if k not in ('binding', 'binding_origin_site')} for s in oracle['sites']]
    for site in expected:
        site['owner_test'] = 4294967295 if site['static_test_id'] is None else site['static_test_id']
        site['owner_bytes_hex'] = site['owner'].encode('ascii').hex()
    assert data['sites'] == expected, ('site identity/span/semantic metadata mismatch', data['sites'], expected)


def static_signature(data):
    # Coverage hit flags vary by enabled mode; identity/denominator fields do not.
    return ([(row['id'], row['file_id'], row['line']) for row in data['coverage']],
            [(file['lines'], file['exclusions']) for file in data['files']], data['tests'], data['sites'])


def run_observed(session, sources, levels):
    validate_positive_sources(session, sources)
    run_ids, evidence_paths, signatures = set(), set(), {}
    for opt in levels:
        for name in NATIVE_NAMES:
            oracle = ORACLES[name]; entry = sources[name]
            for mode_name in modes_for(name):
                mode = mode_oracle(name, mode_name); key = f'{name}-{mode_name}-O{opt}'
                binary = session.artifacts / (key + '.test')
                pre, post = [session.artifacts / (key + suffix) for suffix in ('.pre.ll', '.post.ll')]
                env = dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post))
                dest, result = session.command(key, [session.compiler, 'test', entry, f'-o={opt}', '-o', binary, *flags_for(mode)], expected=mode['expected'], env=env)
                # Driver stderr is separate from the ordinary test image stdout.
                assert_output(subprocess.CompletedProcess([], 0, result.stdout, b''), oracle)
                matches = re.findall(rb'observation: private evidence ([^\r\n]+)', result.stderr); assert len(matches) == 1
                evidence = Path(os.fsdecode(matches[0])); resolved = str(evidence.resolve())
                assert resolved not in evidence_paths and not evidence.is_symlink(); evidence_paths.add(resolved)
                assert binary.is_file() and not binary.is_symlink()
                assert not (evidence / 'driver.json').is_symlink() and not (evidence / 'report').is_symlink()
                metadata, report = (evidence / 'driver.json').read_bytes(), (evidence / 'report').read_bytes()
                assert len(metadata) <= 33554432 and len(report) <= 2097152
                (dest / 'driver.json').write_bytes(metadata); (dest / 'report').write_bytes(report)
                data = support.strict_json(metadata)
                assert data['run'] not in run_ids; run_ids.add(data['run'])
                support.validate_driver(data, report.decode('ascii'), session, binary, entry, expected=mode['expected'],
                    pairs=[tuple(p) for p in oracle['pairs']], hits=[tuple(p) for p in mode['hits']], assertions=oracle['assertions'],
                    tests=oracle['tests'], live=oracle['live_allocations'], byte_count=oracle['tracked_outstanding_bytes'])
                assert data['allocations'] == oracle['allocations']
                assert data['allocation_domains'] == oracle['allocation_domains']
                validate_sites(data, oracle)
                assert data['enabled'] == mode['enabled'] and data['warn_memleak'] is mode['warn']
                assert data['max_leaked_bytes'] == mode['maximum'] and data['min_test_coverage'] == mode['minimum']
                assert re.fullmatch(r'x86_64-(?:unknown|pc)-linux-gnu', data['target'])
                assert data['config_sha256'] == support.config_digest(session.compiler_hash, session.runtime_hash, opt,
                    enabled=mode['enabled'], warn=int(mode['warn']), maximum=mode['maximum'], minimum=mode['minimum'], target=data['target'])[0]
                assert result.stderr.count(b'warning: outstanding tracked allocations at suite endpoint\n') == int(mode['warning'])
                for path in (pre, post):
                    text = path.read_text(); assert '__stasha_obs_init_v4' in text and '__stasha_obs_init_v2' not in text
                signature = static_signature(data)
                if name in signatures: assert signature == signatures[name], 'optimization/mode changed static source identity'
                signatures[name] = signature
                session.cases.append(dict(name=key, stage='observed_native', status='pass', executions=1, run_id=data['run'],
                    input_graph_sha256=data['input_graph_sha256'], map_sha256=data['map_sha256'], config_sha256=data['config_sha256'],
                    binary_sha256=sha(binary), source_sha256=sha(entry), pre_ir_sha256=sha(pre), post_ir_sha256=sha(post),
                    report_sha256=sha(dest / 'report'), driver_sha256=sha(dest / 'driver.json'),
                    expected_driver_exit=mode['expected'], assertions_passed=oracle['assertions'], tests=oracle['tests'],
                    eligible=len(oracle['pairs']), covered=len(mode['hits']), live_allocations=oracle['live_allocations'],
                    tracked_outstanding_bytes=oracle['tracked_outstanding_bytes'], allocation_domains=oracle['allocation_domains']))


def rejection_diagnostic(name, result):
    assert name in REJECTIONS and result.returncode == 1
    assert b'PASS\n' not in result.stdout and b'passed, ' not in result.stdout
    # Remove only the exact expected ordinary driver footer, never a generic
    # compiler/verifier/optimizer/internal failure or a similarly named line.
    footer = re.compile(rb'\(EE\) \[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\] code generation failed\n')
    diagnostics = result.stdout + b'\n' + b''.join(line for line in result.stderr.splitlines(keepends=True) if not footer.fullmatch(line))
    assert not support.POSITIVE_NONLAUNCH_FAILURES.search(diagnostics)
    assert re.search(rb'\.sts:[1-9][0-9]*', result.stderr), 'missing source-owned diagnostic'
    assert not re.search(rb'linking|archiving|link_object|archive_object', result.stderr, re.I)
    if REJECTIONS[name]['diagnostic_owner'] == 'observer':
        assert b'unsupported by observation profile' in result.stderr, ('parser error cannot close observer boundary', name, result.stderr)
    else:
        assert b'unsupported by observation profile' in result.stderr or re.search(rb'(?:make|count|integer|element|cast|type|slice)', result.stderr, re.I)


def run_nonlaunch(session, sources, levels, ir_compiler):
    ir_compiler = ir_compiler.resolve(strict=True); digest = sha(ir_compiler)
    assert ir_compiler != session.compiler and digest != session.compiler_hash
    session.ir_compiler_hash = digest
    for name in REJECTIONS:
        key = 'reject-' + name; source = session.artifacts / ('sources/primitive-slices/reject/' + name + '.sts')
        output = session.artifacts / (key + '.must-not-exist')
        pre, post = [session.artifacts / (key + suffix) for suffix in ('.pre.ll', '.post.ll')]
        dest, result = session.command(key, [ir_compiler, 'test', source, '--warn-memleak', '-o', output], expected=1,
            env=dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post)))
        rejection_diagnostic(name, result); support.retain_nonlaunch_evidence(dest, result)
        assert not output.exists() and not pre.exists() and not post.exists()
        session.cases.append(dict(name=key, stage='compile_only', status='pass', executions=0, diagnostic_owner=REJECTIONS[name]['diagnostic_owner']))
    for opt in levels:
        controls = [(f'{name}-{mode}-ir-O{opt}', sources[name], [] if mode == 'ordinary' else flags_for(mode_oracle(name, mode)), mode != 'ordinary')
                    for name in NATIVE_NAMES for mode in ('ordinary', 'warn', 'combined')]
        controls += [(f'count-{name}-{mode}-ir-O{opt}', session.artifacts / ('sources/primitive-slices/count-failure/' + name + '.sts'),
                      [] if mode == 'ordinary' else ['--warn-memleak', '--min-test-coverage', '0'], mode != 'ordinary')
                     for name in COUNT_FAILURE_IDS for mode in ('ordinary', 'observed')]
        for key, source, flags, observed in controls:
            output = session.artifacts / (key + '.must-not-exist')
            pre, post = [session.artifacts / (key + suffix) for suffix in ('.pre.ll', '.post.ll')]
            dest, result = session.command(key, [ir_compiler, 'test', source, f'-o={opt}', '-o', output, *flags], expected=1,
                env=dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post)))
            support.validate_positive_nonlaunch_result(result, output); support.retain_nonlaunch_evidence(dest, result)
            for path in (pre, post):
                text = path.read_text(); assert ('__stasha_obs_init_v4' in text) == observed
                if not observed: assert '__stasha_obs_' not in text
            session.cases.append(dict(name=key, stage='compile_only', status='pass', executions=0,
                pre_ir_sha256=sha(pre), post_ir_sha256=sha(post), independent_ir_review='required'))
    assert sha(ir_compiler) == digest


def selftest(artifacts):
    class PureTests(unittest.TestCase):
        def test_literal_counts(self):
            self.assertEqual(len(NATIVE_NAMES), 47); self.assertEqual(len(set(NATIVE_NAMES)), 47)
            self.assertEqual(len(REJECTIONS), 174); self.assertEqual(len(COUNT_FAILURE_IDS), 22)
            self.assertEqual(len(SOURCE_HASHES), 247); self.assertEqual(len(GENERATED_SOURCES), 242)
            self.assertEqual(len(expected_cases(range(4))['ordinary_native']), 188)
            self.assertEqual(len(expected_cases(range(4))['observed_native']), 800)
        def test_exact_source_bytes(self):
            for relative, text in GENERATED_SOURCES.items():
                self.assertEqual(hashlib.sha256(text.encode('ascii')).hexdigest(), SOURCE_HASHES[relative])
                self.assertTrue(text.endswith('\n')); self.assertNotIn('\r', text)
            for relative, digest in FIXTURE_HASHES.items(): self.assertEqual(sha(FIXTURES / relative), digest)
        def test_threshold_and_na_oracles(self):
            self.assertEqual(mode_oracle('coverage', 'coverage_66')['expected'], 0)
            self.assertEqual(mode_oracle('coverage', 'coverage_67')['expected'], 1)
            for name in NATIVE_NAMES:
                self.assertEqual(mode_oracle(name, 'max_equal')['expected'], 0)
                self.assertEqual(mode_oracle(name, 'warn')['expected'], 0)
                self.assertEqual(mode_oracle(name, 'coverage_only')['expected'], int(not ORACLES[name]['pairs']))
            for name in BELOW_NAMES: self.assertEqual(mode_oracle(name, 'max_below')['expected'], 1)
            self.assertEqual([ORACLES[n]['tracked_outstanding_bytes'] for n in ('mixed_both','mixed_explicit_only','mixed_slice_only')], [27,7,20])
        def test_events_and_domains(self):
            for name, events in DYNAMIC_EVENTS.items():
                oracle = ORACLES[name]; live = {}
                self.assertEqual([e['bytes'] for e in events if e['operation'] == 'allocate'], oracle['event_bytes'])
                for event in events:
                    if event['operation'] == 'allocate':
                        live[event['generation']] = dict(generation=event['generation'], domain=event['domain'], bytes=event['bytes'],
                            allocation_site=event['site'], resize_site=4294967295, test=event['test'])
                    else:
                        self.assertEqual(live[event['generation']]['allocation_site'], oracle['sites'][event['site']]['binding_origin_site'])
                        del live[event['generation']]
                self.assertEqual(list(live.values()), oracle['allocations'])
                self.assertEqual(sum(d['tracked_outstanding_bytes'] for d in oracle['allocation_domains'].values()), oracle['tracked_outstanding_bytes'])
        def test_static_sites(self):
            for name, oracle in ORACLES.items():
                self.assertEqual([s['id'] for s in oracle['sites']], list(range(len(oracle['sites']))))
                for site in oracle['sites']:
                    self.assertIn(site['kind'], (1,3,4,5))
                    if site['kind'] != 4: self.assertEqual([site[k] for k in ('slice_element_kind','slice_abi_stride','slice_count_arity')], [0,0,0])
                    else: self.assertIn(site['slice_count_arity'], (1,2))
            self.assertNotEqual(ORACLES['same_line']['sites'][0]['col'], ORACLES['same_line']['sites'][1]['col'])
        def test_nonlaunch_partition(self):
            native = sum(expected_cases([0,1,2,3]).values(), [])
            self.assertFalse(set(native) & set(nonlaunch_inventory([0,1,2,3])['compile_only']))
            self.assertTrue(all(not n.startswith(('reject-', 'count-len_', 'count-cap_')) for n in native))
            self.assertEqual(NATIVE_OOM, 'NOT_RUN / outside initial native qualification')
        def test_prepare_is_nonterminal_and_input_pinned(self):
            with tempfile.TemporaryDirectory() as tmp:
                session = support.FamilySession(FAMILY, None, Path(tmp), [0,1,2,3], runtime=True)
                session.nonlaunch_required = nonlaunch_inventory(session.levels)['compile_only']
                sources = materialize(session); validate_positive_sources(session, sources)
                session.write_inventory(expected_cases(session.levels)); session.status = 'prepared'; session.receipt()
                self.assertEqual(session.records, []); self.assertEqual(session.cases, [])
                receipt = json.loads((session.artifacts / 'result.json').read_text())
                self.assertEqual(receipt['status'], 'prepared'); self.assertEqual(receipt['schema'], 2)
                self.assertEqual(receipt['separate_no_launch_controls']['status'], 'NOT_RUN')
                sources['primitive_i8'].write_text('changed')
                self.assertRaises(AssertionError, session.verify_inputs)
        def test_unsafe_source_rejected_before_dispatch(self):
            with tempfile.TemporaryDirectory() as tmp:
                session = support.FamilySession(FAMILY, None, Path(tmp), [0], runtime=True)
                sources = materialize(session)
                for prefix in ('reject/descriptor_copy-called', 'count-failure/len_u64_max'):
                    altered = dict(sources); altered['primitive_i8'] = session.artifacts / ('sources/primitive-slices/' + prefix + '.sts')
                    self.assertRaises(AssertionError, run_ordinary, session, altered, [0])
                    self.assertRaises(AssertionError, run_observed, session, altered, [0])
                self.assertFalse(session.records)
        def test_rejection_diagnostic_owned(self):
            result = subprocess.CompletedProcess([], 1, b'', b'case.sts:3:1: unsupported by observation profile: descriptor escape\n')
            rejection_diagnostic('descriptor_copy-called', result)
            for bad in (b'case.sts:3:1: parse error\n', result.stderr + b'LLVM IR verification failed\n', result.stderr + b'linking failed\n'):
                self.assertRaises(AssertionError, rejection_diagnostic, 'descriptor_copy-called', subprocess.CompletedProcess([], 1, b'', bad))
        def test_static_signature_keeps_identity_not_mode_hits(self):
            data = dict(coverage=[dict(id=0,file_id=0,line=3,hit=False)], files=[dict(lines=[3],exclusions=[1,2,1,2])],tests=[],sites=[])
            before = static_signature(data)
            data['coverage'][0]['hit'] = True
            self.assertEqual(before, static_signature(data))
            data['coverage'][0]['line'] = 4
            self.assertNotEqual(before, static_signature(data))
        def test_site_mismatch_rejected(self):
            oracle = ORACLES['primitive_i32']
            sites = [{k:v for k,v in s.items() if k not in ('binding','binding_origin_site')} for s in oracle['sites']]
            for site in sites:
                site['owner_test'] = 4294967295 if site['static_test_id'] is None else site['static_test_id']
                site['owner_bytes_hex'] = site['owner'].encode('ascii').hex()
            validate_sites({'sites': sites}, oracle)
            for field in ('col','slice_element_kind','slice_abi_stride','slice_count_arity','kind','static_test_id'):
                changed = [dict(s) for s in sites]; changed[0][field] = 999
                self.assertRaises(AssertionError, validate_sites, {'sites': changed}, oracle)
    with mock.patch('subprocess.run', side_effect=AssertionError('pure slice tests cannot spawn processes')):
        result = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(PureTests))
    artifacts.mkdir(parents=True, exist_ok=True)
    (artifacts / 'result.json').write_text(json.dumps(dict(schema=2, family='primitive-slice-selftest',
        status='pass' if result.wasSuccessful() else 'fail', tests_run=result.testsRun, failures=len(result.failures), errors=len(result.errors), native_executions=0), indent=2) + '\n')
    return 0 if result.wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path); parser.add_argument('--ir-compiler', type=Path)
    parser.add_argument('--artifacts', required=True, type=Path); parser.add_argument('--opt', default='0,1,2,3')
    parser.add_argument('--cc', default='cc'); parser.add_argument('--selftest-only', action='store_true')
    parser.add_argument('--stage', choices=('all','prepare','compile-only','native'), default='all')
    args = parser.parse_args()
    if args.selftest_only: return selftest(args.artifacts)
    levels = support.parse_levels(args.opt, parser)
    if args.stage != 'prepare' and args.compiler is None: parser.error('--compiler required')
    if args.stage == 'compile-only' and args.ir_compiler is None: parser.error('--ir-compiler required; normal compiler fallback is forbidden')
    session = support.FamilySession(FAMILY, args.compiler, args.artifacts, levels, runtime=True)
    session.nonlaunch_required = nonlaunch_inventory(levels)['compile_only']
    try:
        sources = materialize(session)
        inventory = nonlaunch_inventory(levels) if args.stage == 'compile-only' else expected_cases(levels)
        session.write_inventory(inventory)
        if args.stage == 'prepare': session.status = 'prepared'
        elif args.stage == 'compile-only': run_nonlaunch(session, sources, levels, args.ir_compiler); session.finish(inventory)
        else: run_ordinary(session, sources, levels); run_observed(session, sources, levels); session.finish(inventory)
        assert sha(session.artifacts / 'literal-oracles.json') == session.oracle_hash
        assert sha(session.artifacts / 'dynamic-events.json') == session.event_hash
    except BaseException as error:
        session.status, session.failure = 'fail', repr(error); raise
    finally: session.receipt()
    print(f'{session.status.upper()}: primitive-slice {args.stage}; evidence {session.artifacts}; native OOM {NATIVE_OOM}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
