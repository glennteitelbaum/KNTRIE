const RAW_DATA = [
  {pattern:"random",N:100,kntrie_int32_find:0.0005,kntrie_int32_insert:0.0022,kntrie_int32_erase:0.0013,kntrie_int32_mem:4816,kntrie_uint64_find:0.0006,kntrie_uint64_insert:0.0045,kntrie_uint64_erase:0.0010,kntrie_uint64_mem:4880,map_int32_find:0.0006,map_int32_insert:0.0036,map_int32_erase:0.0022,map_int32_mem:4800,map_uint64_find:0.0006,map_uint64_insert:0.0040,map_uint64_erase:0.0031,map_uint64_mem:4800,umap_int32_find:0.0005,umap_int32_insert:0.0016,umap_int32_erase:0.0009,umap_int32_mem:3224,umap_uint64_find:0.0005,umap_uint64_insert:0.0017,umap_uint64_erase:0.0011,umap_uint64_mem:3224},
  {pattern:"random",N:150,kntrie_int32_find:0.0007,kntrie_int32_insert:0.0039,kntrie_int32_erase:0.0017,kntrie_int32_mem:5984,kntrie_uint64_find:0.0008,kntrie_uint64_insert:0.0043,kntrie_uint64_erase:0.0018,kntrie_uint64_mem:6208,map_int32_find:0.0009,map_int32_insert:0.0060,map_int32_erase:0.0032,map_int32_mem:7200,map_uint64_find:0.0009,map_uint64_insert:0.0059,map_uint64_erase:0.0040,map_uint64_mem:7200,umap_int32_find:0.0008,umap_int32_insert:0.0056,umap_int32_erase:0.0017,umap_int32_mem:4856,umap_uint64_find:0.0008,umap_uint64_insert:0.0041,umap_uint64_erase:0.0017,umap_uint64_mem:4856},
  {pattern:"random",N:225,kntrie_int32_find:0.0011,kntrie_int32_insert:0.0072,kntrie_int32_erase:0.0025,kntrie_int32_mem:7464,kntrie_uint64_find:0.0010,kntrie_uint64_insert:0.0070,kntrie_uint64_erase:0.0024,kntrie_uint64_mem:8016,map_int32_find:0.0015,map_int32_insert:0.0104,map_int32_erase:0.0058,map_int32_mem:10800,map_uint64_find:0.0015,map_uint64_insert:0.0090,map_uint64_erase:0.0060,map_uint64_mem:10800,umap_int32_find:0.0013,umap_int32_insert:0.0089,umap_int32_erase:0.0022,umap_int32_mem:7216,umap_uint64_find:0.0012,umap_uint64_insert:0.0075,umap_uint64_erase:0.0021,umap_uint64_mem:7216},
  {pattern:"random",N:337,kntrie_int32_find:0.0016,kntrie_int32_insert:0.0123,kntrie_int32_erase:0.0040,kntrie_int32_mem:9488,kntrie_uint64_find:0.0018,kntrie_uint64_insert:0.0119,kntrie_uint64_erase:0.0050,kntrie_uint64_mem:10448,map_int32_find:0.0025,map_int32_insert:0.0190,map_int32_erase:0.0096,map_int32_mem:16176,map_uint64_find:0.0025,map_uint64_insert:0.0206,map_uint64_erase:0.0119,map_uint64_mem:16176,umap_int32_find:0.0018,umap_int32_insert:0.0121,umap_int32_erase:0.0028,umap_int32_mem:10784,umap_uint64_find:0.0018,umap_uint64_insert:0.0106,umap_uint64_erase:0.0032,umap_uint64_mem:10784},
  {pattern:"random",N:506,kntrie_int32_find:0.0024,kntrie_int32_insert:0.0212,kntrie_int32_erase:0.0071,kntrie_int32_mem:12224,kntrie_uint64_find:0.0025,kntrie_uint64_insert:0.0211,kntrie_uint64_erase:0.0077,kntrie_uint64_mem:13664,map_int32_find:0.0040,map_int32_insert:0.0331,map_int32_erase:0.0159,map_int32_mem:24288,map_uint64_find:0.0041,map_uint64_insert:0.0354,map_uint64_erase:0.0208,map_uint64_mem:24288,umap_int32_find:0.0026,umap_int32_insert:0.0161,umap_int32_erase:0.0046,umap_int32_mem:16472,umap_uint64_find:0.0026,umap_uint64_insert:0.0153,umap_uint64_erase:0.0045,umap_uint64_mem:16472},
  {pattern:"random",N:759,kntrie_int32_find:0.0040,kntrie_int32_insert:0.0370,kntrie_int32_erase:0.0128,kntrie_int32_mem:15824,kntrie_uint64_find:0.0039,kntrie_uint64_insert:0.0368,kntrie_uint64_erase:0.0136,kntrie_uint64_mem:18160,map_int32_find:0.0072,map_int32_insert:0.0633,map_int32_erase:0.0331,map_int32_mem:36432,map_uint64_find:0.0075,map_uint64_insert:0.0653,map_uint64_erase:0.0349,map_uint64_mem:36432,umap_int32_find:0.0040,umap_int32_insert:0.0246,umap_int32_erase:0.0066,umap_int32_mem:24304,umap_uint64_find:0.0040,umap_uint64_insert:0.0240,umap_uint64_erase:0.0072,umap_uint64_mem:24304},
  {pattern:"random",N:1139,kntrie_int32_find:0.0061,kntrie_int32_insert:0.0635,kntrie_int32_erase:0.0226,kntrie_int32_mem:20824,kntrie_uint64_find:0.0059,kntrie_uint64_insert:0.0630,kntrie_uint64_erase:0.0251,kntrie_uint64_mem:24544,map_int32_find:0.0165,map_int32_insert:0.1042,map_int32_erase:0.0573,map_int32_mem:54672,map_uint64_find:0.0215,map_uint64_insert:0.1046,map_uint64_erase:0.0664,map_uint64_mem:54672,umap_int32_find:0.0058,umap_int32_insert:0.0376,umap_int32_erase:0.0104,umap_int32_mem:36880,umap_uint64_find:0.0060,umap_uint64_insert:0.0355,umap_uint64_erase:0.0114,umap_uint64_mem:36880},
  {pattern:"random",N:1708,kntrie_int32_find:0.0145,kntrie_int32_insert:0.1110,kntrie_int32_erase:0.0411,kntrie_int32_mem:28320,kntrie_uint64_find:0.0143,kntrie_uint64_insert:0.1048,kntrie_uint64_erase:0.0419,kntrie_uint64_mem:34384,map_int32_find:0.0355,map_int32_insert:0.1786,map_int32_erase:0.0973,map_int32_mem:81984,map_uint64_find:0.0430,map_uint64_insert:0.1855,map_uint64_erase:0.1020,map_uint64_mem:81984,umap_int32_find:0.0094,umap_int32_insert:0.0539,umap_int32_erase:0.0170,umap_int32_mem:54920,umap_uint64_find:0.0094,umap_uint64_insert:0.0560,umap_uint64_erase:0.0201,umap_uint64_mem:54920},
  {pattern:"random",N:2562,kntrie_int32_find:0.0351,kntrie_int32_insert:0.1757,kntrie_int32_erase:0.0666,kntrie_int32_mem:39680,kntrie_uint64_find:0.0371,kntrie_uint64_insert:0.1660,kntrie_uint64_erase:0.0607,kntrie_uint64_mem:49104,map_int32_find:0.0656,map_int32_insert:0.2988,map_int32_erase:0.1595,map_int32_mem:122976,map_uint64_find:0.0704,map_uint64_insert:0.2844,map_uint64_erase:0.1609,map_uint64_mem:122976,umap_int32_find:0.0139,umap_int32_insert:0.0795,umap_int32_erase:0.0314,umap_int32_mem:83512,umap_uint64_find:0.0141,umap_uint64_insert:0.0787,umap_uint64_erase:0.0320,umap_uint64_mem:83512},
  {pattern:"random",N:3844,kntrie_int32_find:0.0688,kntrie_int32_insert:0.2771,kntrie_int32_erase:0.0930,kntrie_int32_mem:56080,kntrie_uint64_find:0.0775,kntrie_uint64_insert:0.2772,kntrie_uint64_erase:0.0966,kntrie_uint64_mem:71840,map_int32_find:0.1160,map_int32_insert:0.4587,map_int32_erase:0.2570,map_int32_mem:184512,map_uint64_find:0.1257,map_uint64_insert:0.4776,map_uint64_erase:0.2622,map_uint64_mem:184512,umap_int32_find:0.0214,umap_int32_insert:0.1243,umap_int32_erase:0.0499,umap_int32_mem:124472,umap_uint64_find:0.0215,umap_uint64_insert:0.1293,umap_uint64_erase:0.0550,umap_uint64_mem:124472},
  {pattern:"random",N:5766,kntrie_int32_find:0.1129,kntrie_int32_insert:0.4109,kntrie_int32_erase:0.1541,kntrie_int32_mem:82336,kntrie_uint64_find:0.1289,kntrie_uint64_insert:0.4159,kntrie_uint64_erase:0.1480,kntrie_uint64_mem:104832,map_int32_find:0.2081,map_int32_insert:0.7571,map_int32_erase:0.4230,map_int32_mem:276768,map_uint64_find:0.2225,map_uint64_insert:0.7910,map_uint64_erase:0.4473,map_uint64_mem:276768,umap_int32_find:0.0433,umap_int32_insert:0.1906,umap_int32_erase:0.0882,umap_int32_mem:186008,umap_uint64_find:0.0333,umap_uint64_insert:0.1889,umap_uint64_erase:0.0943,umap_uint64_mem:186008},
  {pattern:"random",N:8649,kntrie_int32_find:0.2322,kntrie_int32_insert:0.6200,kntrie_int32_erase:0.2219,kntrie_int32_mem:118592,kntrie_uint64_find:0.2472,kntrie_uint64_insert:0.6122,kntrie_uint64_erase:0.2208,kntrie_uint64_mem:156416,map_int32_find:0.3827,map_int32_insert:1.2851,map_int32_erase:0.7174,map_int32_mem:415152,map_uint64_find:0.3870,map_uint64_insert:1.2786,map_uint64_erase:0.7241,map_uint64_mem:415152,umap_int32_find:0.0978,umap_int32_insert:0.2739,umap_int32_erase:0.1358,umap_int32_mem:277840,umap_uint64_find:0.0676,umap_uint64_insert:0.2847,umap_uint64_erase:0.1427,umap_uint64_mem:277840},
  {pattern:"random",N:12974,kntrie_int32_find:0.3927,kntrie_int32_insert:0.9121,kntrie_int32_erase:0.3532,kntrie_int32_mem:177984,kntrie_uint64_find:0.4130,kntrie_uint64_insert:0.9203,kntrie_uint64_erase:0.3661,kntrie_uint64_mem:228608,map_int32_find:0.6735,map_int32_insert:2.0793,map_int32_erase:1.1607,map_int32_mem:622752,map_uint64_find:0.6897,map_uint64_insert:2.1165,map_uint64_erase:1.1889,map_uint64_mem:622752,umap_int32_find:0.1994,umap_int32_insert:0.4332,umap_int32_erase:0.2087,umap_int32_mem:415240,umap_uint64_find:0.1712,umap_uint64_insert:0.4441,umap_uint64_erase:0.2226,umap_uint64_mem:415240},
  {pattern:"random",N:19461,kntrie_int32_find:0.6320,kntrie_int32_insert:1.3806,kntrie_int32_erase:0.5460,kntrie_int32_mem:260736,kntrie_uint64_find:0.6730,kntrie_uint64_insert:1.3906,kntrie_uint64_erase:0.5731,kntrie_uint64_mem:348928,map_int32_find:1.1087,map_int32_insert:3.3627,map_int32_erase:1.8848,map_int32_mem:934128,map_uint64_find:1.1035,map_uint64_insert:3.4388,map_uint64_erase:1.8967,map_uint64_mem:934128,umap_int32_find:0.3227,umap_int32_insert:0.6362,umap_int32_erase:0.3263,umap_int32_mem:633088,umap_uint64_find:0.2820,umap_uint64_insert:0.6509,umap_uint64_erase:0.3213,umap_uint64_mem:633088},
  {pattern:"random",N:29192,kntrie_int32_find:1.0153,kntrie_int32_insert:2.1622,kntrie_int32_erase:0.8769,kntrie_int32_mem:391936,kntrie_uint64_find:1.0914,kntrie_uint64_insert:2.1460,kntrie_uint64_erase:0.9299,kntrie_uint64_mem:511488,map_int32_find:1.8574,map_int32_insert:5.4925,map_int32_erase:3.0292,map_int32_mem:1401216,map_uint64_find:1.9966,map_uint64_insert:5.6431,map_uint64_erase:3.2216,map_uint64_mem:1401216,umap_int32_find:0.4931,umap_int32_insert:0.9357,umap_int32_erase:0.4914,umap_int32_mem:946424,umap_uint64_find:0.4775,umap_uint64_insert:0.9779,umap_uint64_erase:0.5082,umap_uint64_mem:946424},
  {pattern:"random",N:43789,kntrie_int32_find:1.6083,kntrie_int32_insert:3.1446,kntrie_int32_erase:1.4321,kntrie_int32_mem:594432,kntrie_uint64_find:1.7529,kntrie_uint64_insert:3.3209,kntrie_uint64_erase:1.5298,kntrie_uint64_mem:773120,map_int32_find:3.1319,map_int32_insert:9.4090,map_int32_erase:5.3858,map_int32_mem:2101872,map_uint64_find:3.3506,map_uint64_insert:9.7474,map_uint64_erase:5.6187,map_uint64_mem:2101872,umap_int32_find:0.8099,umap_int32_insert:1.4936,umap_int32_erase:0.8106,umap_int32_mem:1414784,umap_uint64_find:0.8145,umap_uint64_insert:1.5554,umap_uint64_erase:0.8651,umap_uint64_mem:1414784},
  {pattern:"random",N:65684,kntrie_int32_find:3.1597,kntrie_int32_insert:5.0172,kntrie_int32_erase:2.4976,kntrie_int32_mem:856576,kntrie_uint64_find:3.3677,kntrie_uint64_insert:5.4132,kntrie_uint64_erase:2.7132,kntrie_uint64_mem:1185792,map_int32_find:5.7143,map_int32_insert:16.1279,map_int32_erase:9.1886,map_int32_mem:3152832,map_uint64_find:6.1680,map_uint64_insert:16.3811,map_uint64_erase:9.2777,map_uint64_mem:3152832,umap_int32_find:1.3734,umap_int32_insert:2.3407,umap_int32_erase:1.3745,umap_int32_mem:2114872,umap_uint64_find:1.3697,umap_uint64_insert:2.4208,umap_uint64_erase:1.3884,umap_uint64_mem:2114872},
  {pattern:"random",N:98523,kntrie_int32_find:4.4961,kntrie_int32_insert:8.8245,kntrie_int32_erase:3.8907,kntrie_int32_mem:1315840,map_int32_find:9.9686,map_int32_insert:27.0193,map_int32_erase:15.3132,map_int32_mem:4729104,umap_int32_find:2.3101,umap_int32_insert:3.9984,umap_int32_erase:2.3489,umap_int32_mem:3162416},
  {pattern:"random",N:98526,kntrie_uint64_find:4.8062,kntrie_uint64_insert:9.1886,kntrie_uint64_erase:3.9903,kntrie_uint64_mem:1712128,map_uint64_find:9.6054,map_uint64_insert:26.3317,map_uint64_erase:15.4520,map_uint64_mem:4729248,umap_uint64_find:2.2575,umap_uint64_insert:4.1816,umap_uint64_erase:2.3592,umap_uint64_mem:3162488},
  {pattern:"random",N:147785,kntrie_int32_find:7.2489,kntrie_int32_insert:14.8824,kntrie_int32_erase:5.5471,kntrie_int32_mem:1890304,map_int32_find:18.0619,map_int32_insert:51.4173,map_int32_erase:28.1705,map_int32_mem:7093680,umap_int32_find:3.7047,umap_int32_insert:6.8738,umap_int32_erase:3.8531,umap_int32_mem:4729184},
  {pattern:"random",N:147789,kntrie_uint64_find:8.0780,kntrie_uint64_insert:15.8006,kntrie_uint64_erase:5.7524,kntrie_uint64_mem:2623488,map_uint64_find:18.1984,map_uint64_insert:52.6245,map_uint64_erase:27.9846,map_uint64_mem:7093872,umap_uint64_find:3.6653,umap_uint64_insert:7.0578,umap_uint64_erase:3.8918,umap_uint64_mem:4729280},
  {pattern:"random",N:221675,kntrie_int32_find:11.5879,kntrie_int32_insert:24.9863,kntrie_int32_erase:8.7148,kntrie_int32_mem:2985984,map_int32_find:32.8263,map_int32_insert:90.6895,map_int32_erase:48.8771,map_int32_mem:10640400,umap_int32_find:5.6718,umap_int32_insert:10.8943,umap_int32_erase:6.0035,umap_int32_mem:7215376},
  {pattern:"random",N:221683,kntrie_uint64_find:13.5556,kntrie_uint64_insert:26.8794,kntrie_uint64_erase:9.4724,kntrie_uint64_mem:3756032,map_uint64_find:32.7470,map_uint64_insert:85.7240,map_uint64_erase:48.0247,map_uint64_mem:10640784,umap_uint64_find:5.9129,umap_uint64_insert:13.8168,umap_uint64_erase:6.4984,umap_uint64_mem:7215568},
  {pattern:"random",N:332509,kntrie_int32_find:18.3674,kntrie_int32_insert:40.5566,kntrie_int32_erase:13.4187,kntrie_int32_mem:4247552,map_int32_find:56.9451,map_int32_insert:158.7703,map_int32_erase:84.1619,map_int32_mem:15960432,umap_int32_find:9.5849,umap_int32_insert:18.3457,umap_int32_erase:9.9877,umap_int32_mem:10788704},
  {pattern:"random",N:332525,kntrie_uint64_find:25.1881,kntrie_uint64_insert:47.9298,kntrie_uint64_erase:16.3514,kntrie_uint64_mem:5982208,map_uint64_find:58.8553,map_uint64_insert:169.8174,map_uint64_erase:92.1989,map_uint64_mem:15961200,umap_uint64_find:9.5850,umap_uint64_insert:19.3119,umap_uint64_erase:10.2813,umap_uint64_mem:10789088},
  {pattern:"random",N:498753,kntrie_int32_find:30.5112,kntrie_int32_insert:66.2917,kntrie_int32_erase:22.4996,kntrie_int32_mem:6318080,map_int32_find:112.9281,map_int32_insert:302.7540,map_int32_erase:154.7817,map_int32_mem:23940144,umap_int32_find:15.2844,umap_int32_insert:30.5403,umap_int32_erase:16.1203,umap_int32_mem:16132000},
  {pattern:"random",N:498788,kntrie_uint64_find:41.6597,kntrie_uint64_insert:78.6428,kntrie_uint64_erase:26.4712,kntrie_uint64_mem:8423424,map_uint64_find:118.9597,map_uint64_insert:291.1524,map_uint64_erase:158.3003,map_uint64_mem:23941824,umap_uint64_find:15.2047,umap_uint64_insert:32.5425,umap_uint64_erase:16.6646,umap_uint64_mem:16132840},
  {pattern:"random",N:748117,kntrie_int32_find:59.5225,kntrie_int32_insert:117.2267,kntrie_int32_erase:38.6502,kntrie_int32_mem:10487808,map_int32_find:201.6860,map_int32_insert:545.1316,map_int32_erase:279.9513,map_int32_mem:35909616,umap_int32_find:24.3850,umap_int32_insert:48.3881,umap_int32_erase:25.9162,umap_int32_mem:24123200},
  {pattern:"random",N:748182,kntrie_uint64_find:77.8887,kntrie_uint64_insert:135.7427,kntrie_uint64_erase:46.8642,kntrie_uint64_mem:12584960,map_uint64_find:196.6030,map_uint64_insert:549.8420,map_uint64_erase:286.6129,map_uint64_mem:35912736,umap_uint64_find:23.9819,umap_uint64_insert:48.4159,umap_uint64_erase:25.5261,umap_uint64_mem:24124760},
  {pattern:"random",N:1122145,kntrie_int32_find:78.1729,kntrie_int32_insert:205.0346,kntrie_int32_erase:60.1199,kntrie_int32_mem:14016456,umap_int32_find:40.0625,umap_int32_insert:85.3098,umap_int32_erase:41.7178,umap_int32_mem:36074048},
  {pattern:"random",N:1122274,kntrie_uint64_find:84.5431,kntrie_uint64_insert:249.4722,kntrie_uint64_erase:66.2739,kntrie_uint64_mem:20960080,umap_uint64_find:39.9888,umap_uint64_insert:90.7438,umap_uint64_erase:41.8894,umap_uint64_mem:36077144},
  {pattern:"random",N:1683111,kntrie_int32_find:120.6338,kntrie_int32_insert:284.6193,kntrie_int32_erase:106.1123,kntrie_int32_mem:20232320,umap_int32_find:64.8396,umap_int32_insert:140.3812,umap_int32_erase:67.8800,umap_int32_mem:53945536},
  {pattern:"random",N:1683411,kntrie_uint64_find:129.4416,kntrie_uint64_insert:321.8498,kntrie_uint64_erase:107.9551,kntrie_uint64_mem:30630784,umap_uint64_find:68.7736,umap_uint64_insert:186.9469,umap_uint64_erase:83.0762,umap_uint64_mem:53952736},
  {pattern:"random",N:2524384,kntrie_int32_find:194.7170,kntrie_int32_insert:405.0088,kntrie_int32_erase:166.8388,kntrie_int32_mem:29209120,umap_int32_find:110.1608,umap_int32_insert:318.3854,umap_int32_erase:142.3864,umap_int32_mem:82315208},
  {pattern:"random",N:2525116,kntrie_uint64_find:223.2492,kntrie_uint64_insert:456.1175,kntrie_uint64_erase:191.0539,kntrie_uint64_mem:45663616,umap_uint64_find:111.8068,umap_uint64_insert:331.4377,umap_uint64_erase:148.3348,umap_uint64_mem:82332776},
  {pattern:"random",N:3786033,kntrie_int32_find:326.1733,kntrie_int32_insert:596.0262,kntrie_int32_erase:281.7438,kntrie_int32_mem:43641664,umap_int32_find:184.7136,umap_int32_insert:548.8767,umap_int32_erase:252.1117,umap_int32_mem:123073040},
  {pattern:"random",N:3787675,kntrie_uint64_find:380.5881,kntrie_uint64_insert:672.7007,kntrie_uint64_erase:309.0574,kntrie_uint64_mem:67562560,umap_uint64_find:189.2663,umap_uint64_insert:599.7011,umap_uint64_erase:283.4183,umap_uint64_mem:123112448},
  {pattern:"random",N:5677748,kntrie_int32_find:558.2332,kntrie_int32_insert:940.8382,kntrie_int32_erase:481.9242,kntrie_int32_mem:63047680,umap_int32_find:335.1832,umap_int32_insert:994.7115,umap_int32_erase:495.1920,umap_int32_mem:184004728},
  {pattern:"random",N:5681512,kntrie_uint64_find:662.1377,kntrie_uint64_insert:1093.5502,kntrie_uint64_erase:550.8426,kntrie_uint64_mem:100474112,umap_uint64_find:319.7997,umap_uint64_insert:990.6196,umap_uint64_erase:459.6989,umap_uint64_mem:184095064},
  {pattern:"sequential",N:100,kntrie_int32_find:0.0010,kntrie_int32_insert:0.0048,kntrie_int32_erase:0.0025,kntrie_int32_mem:3328,kntrie_uint64_find:0.0009,kntrie_uint64_insert:0.0043,kntrie_uint64_erase:0.0015,kntrie_uint64_mem:3840,map_int32_find:0.0006,map_int32_insert:0.0062,map_int32_erase:0.0028,map_int32_mem:4800,map_uint64_find:0.0005,map_uint64_insert:0.0031,map_uint64_erase:0.0025,map_uint64_mem:4800,umap_int32_find:0.0003,umap_int32_insert:0.0014,umap_int32_erase:0.0008,umap_int32_mem:3224,umap_uint64_find:0.0003,umap_uint64_insert:0.0014,umap_uint64_erase:0.0007,umap_uint64_mem:3224},
  {pattern:"sequential",N:150,kntrie_int32_find:0.0016,kntrie_int32_insert:0.0093,kntrie_int32_erase:0.0041,kntrie_int32_mem:4096,kntrie_uint64_find:0.0015,kntrie_uint64_insert:0.0062,kntrie_uint64_erase:0.0029,kntrie_uint64_mem:4608,map_int32_find:0.0009,map_int32_insert:0.0104,map_int32_erase:0.0033,map_int32_mem:7200,map_uint64_find:0.0009,map_uint64_insert:0.0055,map_uint64_erase:0.0039,map_uint64_mem:7200,umap_int32_find:0.0005,umap_int32_insert:0.0042,umap_int32_erase:0.0013,umap_int32_mem:4856,umap_uint64_find:0.0005,umap_uint64_insert:0.0039,umap_uint64_erase:0.0011,umap_uint64_mem:4856},
  {pattern:"sequential",N:225,kntrie_int32_find:0.0026,kntrie_int32_insert:0.0102,kntrie_int32_erase:0.0045,kntrie_int32_mem:5120,kntrie_uint64_find:0.0025,kntrie_uint64_insert:0.0104,kntrie_uint64_erase:0.0065,kntrie_uint64_mem:6144,map_int32_find:0.0014,map_int32_insert:0.0091,map_int32_erase:0.0043,map_int32_mem:10800,map_uint64_find:0.0015,map_uint64_insert:0.0095,map_uint64_erase:0.0058,map_uint64_mem:10800,umap_int32_find:0.0008,umap_int32_insert:0.0054,umap_int32_erase:0.0016,umap_int32_mem:7216,umap_uint64_find:0.0008,umap_uint64_insert:0.0055,umap_uint64_erase:0.0016,umap_uint64_mem:7216},
  {pattern:"sequential",N:337,kntrie_int32_find:0.0040,kntrie_int32_insert:0.0222,kntrie_int32_erase:0.0091,kntrie_int32_mem:6144,kntrie_uint64_find:0.0040,kntrie_uint64_insert:0.0224,kntrie_uint64_erase:0.0089,kntrie_uint64_mem:8192,map_int32_find:0.0023,map_int32_insert:0.0206,map_int32_erase:0.0102,map_int32_mem:16176,map_uint64_find:0.0024,map_uint64_insert:0.0205,map_uint64_erase:0.0115,map_uint64_mem:16176,umap_int32_find:0.0012,umap_int32_insert:0.0081,umap_int32_erase:0.0023,umap_int32_mem:10784,umap_uint64_find:0.0012,umap_uint64_insert:0.0080,umap_uint64_erase:0.0022,umap_uint64_mem:10784},
  {pattern:"sequential",N:506,kntrie_int32_find:0.0065,kntrie_int32_insert:0.0409,kntrie_int32_erase:0.0149,kntrie_int32_mem:8192,kntrie_uint64_find:0.0063,kntrie_uint64_insert:0.0440,kntrie_uint64_erase:0.0147,kntrie_uint64_mem:10240,map_int32_find:0.0041,map_int32_insert:0.0328,map_int32_erase:0.0155,map_int32_mem:24288,map_uint64_find:0.0040,map_uint64_insert:0.0313,map_uint64_erase:0.0170,map_uint64_mem:24288,umap_int32_find:0.0018,umap_int32_insert:0.0119,umap_int32_erase:0.0030,umap_int32_mem:16472,umap_uint64_find:0.0018,umap_uint64_insert:0.0116,umap_uint64_erase:0.0033,umap_uint64_mem:16472},
  {pattern:"sequential",N:759,kntrie_int32_find:0.0104,kntrie_int32_insert:0.0736,kntrie_int32_erase:0.0240,kntrie_int32_mem:12288,kntrie_uint64_find:0.0101,kntrie_uint64_insert:0.0823,kntrie_uint64_erase:0.0245,kntrie_uint64_mem:14336,map_int32_find:0.0072,map_int32_insert:0.0641,map_int32_erase:0.0316,map_int32_mem:36432,map_uint64_find:0.0079,map_uint64_insert:0.0603,map_uint64_erase:0.0392,map_uint64_mem:36432,umap_int32_find:0.0026,umap_int32_insert:0.0167,umap_int32_erase:0.0049,umap_int32_mem:24304,umap_uint64_find:0.0026,umap_uint64_insert:0.0166,umap_uint64_erase:0.0049,umap_uint64_mem:24304},
  {pattern:"sequential",N:1139,kntrie_int32_find:0.0170,kntrie_int32_insert:0.1192,kntrie_int32_erase:0.0368,kntrie_int32_mem:16384,kntrie_uint64_find:0.0241,kntrie_uint64_insert:0.1205,kntrie_uint64_erase:0.0379,kntrie_uint64_mem:22528,map_int32_find:0.0176,map_int32_insert:0.1105,map_int32_erase:0.0618,map_int32_mem:54672,map_uint64_find:0.0218,map_uint64_insert:0.1044,map_uint64_erase:0.0643,map_uint64_mem:54672,umap_int32_find:0.0039,umap_int32_insert:0.0244,umap_int32_erase:0.0073,umap_int32_mem:36880,umap_uint64_find:0.0039,umap_uint64_insert:0.0260,umap_uint64_erase:0.0068,umap_uint64_mem:36880},
  {pattern:"sequential",N:1708,kntrie_int32_find:0.0465,kntrie_int32_insert:0.2108,kntrie_int32_erase:0.0597,kntrie_int32_mem:26624,kntrie_uint64_find:0.0493,kntrie_uint64_insert:0.2032,kntrie_uint64_erase:0.0589,kntrie_uint64_mem:30720,map_int32_find:0.0345,map_int32_insert:0.1727,map_int32_erase:0.0958,map_int32_mem:81984,map_uint64_find:0.0414,map_uint64_insert:0.1718,map_uint64_erase:0.1036,map_uint64_mem:81984,umap_int32_find:0.0059,umap_int32_insert:0.0344,umap_int32_erase:0.0105,umap_int32_mem:54920,umap_uint64_find:0.0060,umap_uint64_insert:0.0365,umap_uint64_erase:0.0106,umap_uint64_mem:54920},
  {pattern:"sequential",N:2562,kntrie_int32_find:0.0889,kntrie_int32_insert:0.3264,kntrie_int32_erase:0.0877,kntrie_int32_mem:34816,kntrie_uint64_find:0.0930,kntrie_uint64_insert:0.3527,kntrie_uint64_erase:0.0900,kntrie_uint64_mem:51200,map_int32_find:0.0657,map_int32_insert:0.2847,map_int32_erase:0.1535,map_int32_mem:122976,map_uint64_find:0.0731,map_uint64_insert:0.3026,map_uint64_erase:0.1671,map_uint64_mem:122976,umap_int32_find:0.0089,umap_int32_insert:0.0500,umap_int32_erase:0.0156,umap_int32_mem:83512,umap_uint64_find:0.0089,umap_uint64_insert:0.0514,umap_uint64_erase:0.0155,umap_uint64_mem:83512},
  {pattern:"sequential",N:3844,kntrie_int32_find:0.1638,kntrie_int32_insert:0.4939,kntrie_int32_erase:0.1374,kntrie_int32_mem:51200,kntrie_uint64_find:0.1640,kntrie_uint64_insert:0.5192,kntrie_uint64_erase:0.1338,kntrie_uint64_mem:67584,map_int32_find:0.1187,map_int32_insert:0.4702,map_int32_erase:0.2662,map_int32_mem:184512,map_uint64_find:0.1287,map_uint64_insert:0.4739,map_uint64_erase:0.2963,map_uint64_mem:184512,umap_int32_find:0.0134,umap_int32_insert:0.0738,umap_int32_erase:0.0246,umap_int32_mem:124472,umap_uint64_find:0.0135,umap_uint64_insert:0.0732,umap_uint64_erase:0.0256,umap_uint64_mem:124472},
  {pattern:"sequential",N:5766,kntrie_int32_find:0.0471,kntrie_int32_insert:0.7057,kntrie_int32_erase:0.1085,kntrie_int32_mem:59904,kntrie_uint64_find:0.0586,kntrie_uint64_insert:0.7492,kntrie_uint64_erase:0.1193,kntrie_uint64_mem:59904,map_int32_find:0.2226,map_int32_insert:0.7854,map_int32_erase:0.4190,map_int32_mem:276768,map_uint64_find:0.2282,map_uint64_insert:0.7681,map_uint64_erase:0.4495,map_uint64_mem:276768,umap_int32_find:0.0202,umap_int32_insert:0.1059,umap_int32_erase:0.0375,umap_int32_mem:186008,umap_uint64_find:0.0203,umap_uint64_insert:0.1079,umap_uint64_erase:0.0381,umap_uint64_mem:186008},
  {pattern:"sequential",N:8649,kntrie_int32_find:0.0677,kntrie_int32_insert:0.7972,kntrie_int32_erase:0.1691,kntrie_int32_mem:88704,kntrie_uint64_find:0.0909,kntrie_uint64_insert:0.8589,kntrie_uint64_erase:0.1836,kntrie_uint64_mem:88704,map_int32_find:0.3773,map_int32_insert:1.2993,map_int32_erase:0.7066,map_int32_mem:415152,map_uint64_find:0.3877,map_uint64_insert:1.2804,map_uint64_erase:0.7523,map_uint64_mem:415152,umap_int32_find:0.0305,umap_int32_insert:0.1609,umap_int32_erase:0.0553,umap_int32_mem:277840,umap_uint64_find:0.0302,umap_uint64_insert:0.1595,umap_uint64_erase:0.0562,umap_uint64_mem:277840},
  {pattern:"sequential",N:12974,kntrie_int32_find:0.1043,kntrie_int32_insert:1.0657,kntrie_int32_erase:0.2737,kntrie_int32_mem:132096,kntrie_uint64_find:0.1318,kntrie_uint64_insert:1.0765,kntrie_uint64_erase:0.2921,kntrie_uint64_mem:132096,map_int32_find:0.6499,map_int32_insert:2.1384,map_int32_erase:1.1904,map_int32_mem:622752,map_uint64_find:0.6506,map_uint64_insert:2.0982,map_uint64_erase:1.1905,map_uint64_mem:622752,umap_int32_find:0.0468,umap_int32_insert:0.2360,umap_int32_erase:0.0891,umap_int32_mem:415240,umap_uint64_find:0.0448,umap_uint64_insert:0.2387,umap_uint64_erase:0.0827,umap_uint64_mem:415240},
  {pattern:"sequential",N:19461,kntrie_int32_find:0.1712,kntrie_int32_insert:1.3134,kntrie_int32_erase:0.4198,kntrie_int32_mem:197472,kntrie_uint64_find:0.2013,kntrie_uint64_insert:1.4099,kntrie_uint64_erase:0.4440,kntrie_uint64_mem:197472,map_int32_find:1.0745,map_int32_insert:3.4373,map_int32_erase:1.8689,map_int32_mem:934128,map_uint64_find:1.0760,map_uint64_insert:3.3425,map_uint64_erase:1.8793,map_uint64_mem:934128,umap_int32_find:0.0688,umap_int32_insert:0.3481,umap_int32_erase:0.1272,umap_int32_mem:633088,umap_uint64_find:0.0672,umap_uint64_insert:0.3537,umap_uint64_erase:0.1459,umap_uint64_mem:633088},
  {pattern:"sequential",N:29192,kntrie_int32_find:0.2465,kntrie_int32_insert:1.8870,kntrie_int32_erase:0.6520,kntrie_int32_mem:295024,kntrie_uint64_find:0.3290,kntrie_uint64_insert:1.9794,kntrie_uint64_erase:0.6922,kntrie_uint64_mem:295024,map_int32_find:1.9211,map_int32_insert:5.7128,map_int32_erase:3.1242,map_int32_mem:1401216,map_uint64_find:1.9606,map_uint64_insert:5.5907,map_uint64_erase:3.1091,map_uint64_mem:1401216,umap_int32_find:0.1103,umap_int32_insert:0.5261,umap_int32_erase:0.2040,umap_int32_mem:946424,umap_uint64_find:0.1049,umap_uint64_insert:0.5306,umap_uint64_erase:0.1978,umap_uint64_mem:946424},
  {pattern:"sequential",N:43789,kntrie_int32_find:0.3918,kntrie_int32_insert:2.5885,kntrie_int32_erase:1.0119,kntrie_int32_mem:441504,kntrie_uint64_find:0.5192,kntrie_uint64_insert:2.7946,kntrie_uint64_erase:1.0890,kntrie_uint64_mem:441504,map_int32_find:3.1917,map_int32_insert:9.3695,map_int32_erase:5.2509,map_int32_mem:2101872,map_uint64_find:3.1827,map_uint64_insert:9.2083,map_uint64_erase:5.3380,map_uint64_mem:2101872,umap_int32_find:0.1873,umap_int32_insert:0.7615,umap_int32_erase:0.2927,umap_int32_mem:1414784,umap_uint64_find:0.1790,umap_uint64_insert:0.7912,umap_uint64_erase:0.2949,umap_uint64_mem:1414784},
  {pattern:"sequential",N:65684,kntrie_int32_find:0.8826,kntrie_int32_insert:4.4220,kntrie_int32_erase:1.8760,kntrie_int32_mem:661584,kntrie_uint64_find:1.0509,kntrie_uint64_insert:4.6103,kntrie_uint64_erase:1.9517,kntrie_uint64_mem:661584,map_int32_find:5.5390,map_int32_insert:15.7887,map_int32_erase:8.9058,map_int32_mem:3152832,map_uint64_find:5.8377,map_uint64_insert:16.1380,map_uint64_erase:9.2862,map_uint64_mem:3152832,umap_int32_find:0.3875,umap_int32_insert:1.1829,umap_int32_erase:0.4734,umap_int32_mem:2114872,umap_uint64_find:0.3931,umap_uint64_insert:1.2150,umap_uint64_erase:0.4815,umap_uint64_mem:2114872},
  {pattern:"sequential",N:98526,kntrie_int32_find:1.3745,kntrie_int32_insert:7.0038,kntrie_int32_erase:2.8763,kntrie_int32_mem:991056,kntrie_uint64_find:1.6596,kntrie_uint64_insert:7.3287,kntrie_uint64_erase:3.0032,kntrie_uint64_mem:991056,map_int32_find:9.5364,map_int32_insert:26.5679,map_int32_erase:15.0081,map_int32_mem:4729248,map_uint64_find:9.8418,map_uint64_insert:28.1177,map_uint64_erase:15.6042,map_uint64_mem:4729248,umap_int32_find:0.7277,umap_int32_insert:1.7851,umap_int32_erase:0.8296,umap_int32_mem:3162488,umap_uint64_find:0.6342,umap_uint64_insert:1.8037,umap_uint64_erase:0.7770,umap_uint64_mem:3162488},
  {pattern:"sequential",N:147789,kntrie_int32_find:2.1497,kntrie_int32_insert:10.6894,kntrie_int32_erase:4.4190,kntrie_int32_mem:1485776,kntrie_uint64_find:2.8228,kntrie_uint64_insert:11.1435,kntrie_uint64_erase:4.6069,kntrie_uint64_mem:1485776,map_int32_find:17.8676,map_int32_insert:46.2736,map_int32_erase:26.4715,map_int32_mem:7093872,map_uint64_find:17.4999,map_uint64_insert:51.3608,map_uint64_erase:27.7005,map_uint64_mem:7093872,umap_int32_find:1.1873,umap_int32_insert:3.0032,umap_int32_erase:1.3920,umap_int32_mem:4729280,umap_uint64_find:1.1097,umap_uint64_insert:3.0144,umap_uint64_erase:1.3635,umap_uint64_mem:4729280},
  {pattern:"sequential",N:221683,kntrie_int32_find:3.7550,kntrie_int32_insert:16.2731,kntrie_int32_erase:6.8750,kntrie_int32_mem:2227168,kntrie_uint64_find:4.4440,kntrie_uint64_insert:16.8423,kntrie_uint64_erase:7.1811,kntrie_uint64_mem:2227168,map_int32_find:33.7049,map_int32_insert:90.4865,map_int32_erase:49.0608,map_int32_mem:10640784,map_uint64_find:32.6671,map_uint64_insert:93.3529,map_uint64_erase:50.6428,map_uint64_mem:10640784,umap_int32_find:2.1297,umap_int32_insert:5.1579,umap_int32_erase:2.3555,umap_int32_mem:7215568,umap_uint64_find:1.9958,umap_uint64_insert:4.9300,umap_uint64_erase:2.2142,umap_uint64_mem:7215568},
  {pattern:"sequential",N:332525,kntrie_int32_find:6.7048,kntrie_int32_insert:24.9210,kntrie_int32_erase:11.0682,kntrie_int32_mem:3340112,kntrie_uint64_find:7.9490,kntrie_uint64_insert:26.0357,kntrie_uint64_erase:11.5552,kntrie_uint64_mem:3340112,map_int32_find:61.8941,map_int32_insert:167.3061,map_int32_erase:87.6355,map_int32_mem:15961200,map_uint64_find:61.4678,map_uint64_insert:158.6649,map_uint64_erase:85.7991,map_uint64_mem:15961200,umap_int32_find:3.4325,umap_int32_insert:7.8998,umap_int32_erase:3.7862,umap_int32_mem:10789088,umap_uint64_find:3.6781,umap_uint64_insert:8.5952,umap_uint64_erase:3.7592,umap_uint64_mem:10789088},
  {pattern:"sequential",N:498788,kntrie_int32_find:10.8021,kntrie_int32_insert:39.3478,kntrie_int32_erase:18.5194,kntrie_int32_mem:5009408,kntrie_uint64_find:13.2578,kntrie_uint64_insert:41.3597,kntrie_uint64_erase:19.2733,kntrie_uint64_mem:5009408,map_int32_find:118.7775,map_int32_insert:314.8737,map_int32_erase:157.2472,map_int32_mem:23941824,map_uint64_find:107.8746,map_uint64_insert:286.8254,map_uint64_erase:152.9372,map_uint64_mem:23941824,umap_int32_find:5.8059,umap_int32_insert:13.5174,umap_int32_erase:5.5484,umap_int32_mem:16132840,umap_uint64_find:5.6326,umap_uint64_insert:12.2541,umap_uint64_erase:5.3847,umap_uint64_mem:16132840},
  {pattern:"sequential",N:748182,kntrie_int32_find:17.5228,kntrie_int32_insert:63.5489,kntrie_int32_erase:31.0863,kntrie_int32_mem:7512992,kntrie_uint64_find:23.0761,kntrie_uint64_insert:66.1509,kntrie_uint64_erase:32.7125,kntrie_uint64_mem:7512992,map_int32_find:205.3373,map_int32_insert:538.4129,map_int32_erase:281.8108,map_int32_mem:35912736,map_uint64_find:205.2450,map_uint64_insert:564.7941,map_uint64_erase:293.7516,map_uint64_mem:35912736,umap_int32_find:9.3071,umap_int32_insert:18.9791,umap_int32_erase:8.5739,umap_int32_mem:24124760,umap_uint64_find:9.2023,umap_uint64_insert:20.2560,umap_uint64_erase:8.3669,umap_uint64_mem:24124760},
  {pattern:"sequential",N:1122274,kntrie_int32_find:30.1447,kntrie_int32_insert:107.3616,kntrie_int32_erase:55.1153,kntrie_int32_mem:11268640,kntrie_uint64_find:38.0602,kntrie_uint64_insert:114.3462,kntrie_uint64_erase:59.5652,kntrie_uint64_mem:11268640,umap_int32_find:16.0385,umap_int32_insert:32.2499,umap_int32_erase:14.8872,umap_int32_mem:36077144,umap_uint64_find:15.4698,umap_uint64_insert:32.0185,umap_uint64_erase:13.7542,umap_uint64_mem:36077144},
  {pattern:"sequential",N:1683411,kntrie_int32_find:53.7295,kntrie_int32_insert:179.1196,kntrie_int32_erase:96.4710,kntrie_int32_mem:16901696,kntrie_uint64_find:63.3364,kntrie_uint64_insert:181.9720,kntrie_uint64_erase:96.0122,kntrie_uint64_mem:16901696,umap_int32_find:30.9964,umap_int32_insert:52.4941,umap_int32_erase:22.7342,umap_int32_mem:53952736,umap_uint64_find:26.7238,umap_uint64_insert:53.2127,umap_uint64_erase:23.1967,umap_uint64_mem:53952736},
  {pattern:"sequential",N:2525116,kntrie_int32_find:86.3497,kntrie_int32_insert:290.3758,kntrie_int32_erase:156.1843,kntrie_int32_mem:25352064,kntrie_uint64_find:103.4578,kntrie_uint64_insert:302.6433,kntrie_uint64_erase:165.1469,kntrie_uint64_mem:25352064,umap_int32_find:54.1128,umap_int32_insert:88.8265,umap_int32_erase:39.1276,umap_int32_mem:82332776,umap_uint64_find:57.1081,umap_uint64_insert:112.5304,umap_uint64_erase:40.0861,umap_uint64_mem:82332776},
  {pattern:"sequential",N:3787675,kntrie_int32_find:156.7551,kntrie_int32_insert:482.7194,kntrie_int32_erase:268.4806,kntrie_int32_mem:38027136,kntrie_uint64_find:210.7448,kntrie_uint64_insert:492.5919,kntrie_uint64_erase:272.8705,kntrie_uint64_mem:38027136,umap_int32_find:93.6807,umap_int32_insert:171.5410,umap_int32_erase:63.2187,umap_int32_mem:123112448,umap_uint64_find:95.6875,umap_uint64_insert:187.1087,umap_uint64_erase:62.2954,umap_uint64_mem:123112448},
  {pattern:"sequential",N:5681512,kntrie_int32_find:365.2720,kntrie_int32_insert:799.3450,kntrie_int32_erase:444.2511,kntrie_int32_mem:57039488,kntrie_uint64_find:363.6695,kntrie_uint64_insert:817.7908,kntrie_uint64_erase:449.4102,kntrie_uint64_mem:57039488,umap_int32_find:173.8434,umap_int32_insert:404.3039,umap_int32_erase:126.2438,umap_int32_mem:184095064,umap_uint64_find:179.2531,umap_uint64_insert:429.2135,umap_uint64_erase:147.2073,umap_uint64_mem:184095064},
];

import { useState } from "react";
import { LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer, Legend } from "recharts";

const LINES = [
  { key: "kntrie_int32", color: "#3b82f6", dash: "", label: "kntrie i32" },
  { key: "kntrie_uint64", color: "#93c5fd", dash: "6 3", label: "kntrie u64" },
  { key: "map_int32", color: "#ef4444", dash: "", label: "map i32" },
  { key: "map_uint64", color: "#fca5a5", dash: "6 3", label: "map u64" },
  { key: "umap_int32", color: "#22c55e", dash: "", label: "umap i32" },
  { key: "umap_uint64", color: "#86efac", dash: "6 3", label: "umap u64" },
  { key: "raw_int32", color: "#888", dash: "", label: "raw i32", memOnly: true },
  { key: "raw_uint64", color: "#555", dash: "3 3", label: "raw u64", memOnly: true },
];

const METRICS = [
  { suffix: "find", label: "Find (ns/entry)", convert: (ms, n) => (ms * 1e6) / n },
  { suffix: "insert", label: "Insert (ns/entry)", convert: (ms, n) => (ms * 1e6) / n },
  { suffix: "erase", label: "Erase N/2 (ns/entry)", convert: (ms, n) => (ms * 1e6) / (n / 2) },
  { suffix: "mem", label: "Memory (B/entry)", convert: (b, n) => b / n },
];

const PATTERNS = ["random", "sequential"];

function buildData(pattern, metric) {
  const lines = LINES.filter((l) => metric.suffix === "mem" || !l.memOnly);
  return RAW_DATA
    .filter((r) => r.pattern === pattern)
    .map((r) => {
      const out = { N: r.N, logN: Math.log10(r.N) };
      for (const line of lines) {
        if (line.key === "raw_int32") {
          if (metric.suffix === "mem") out[line.key] = r.N * 12 / r.N;
        } else if (line.key === "raw_uint64") {
          if (metric.suffix === "mem") out[line.key] = r.N * 16 / r.N;
        } else {
          const raw = r[`${line.key}_${metric.suffix}`];
          if (raw != null) out[line.key] = metric.convert(raw, r.N);
        }
      }
      return out;
    });
}

const fmtN = (logV) => {
  const v = Math.pow(10, logV);
  if (v >= 1e6) return `${(v / 1e6).toFixed(v >= 1e7 ? 0 : 1)}M`;
  if (v >= 1e3) return `${(v / 1e3).toFixed(v >= 1e4 ? 0 : 1)}K`;
  return `${Math.round(v)}`;
};

const fmtVal = (v) => {
  if (v == null) return "";
  if (v < 0.1) return v.toFixed(3);
  if (v < 10) return v.toFixed(2);
  if (v < 1000) return v.toFixed(1);
  return v.toFixed(0);
};

const Tip = ({ active, payload, label }) => {
  if (!active || !payload?.length) return null;
  return (
    <div style={{ background: "#1a1a2e", border: "1px solid #444", borderRadius: 8, padding: "8px 12px", fontSize: 12 }}>
      <div style={{ color: "#aaa", marginBottom: 4, fontWeight: 600 }}>N = {fmtN(label)}</div>
      {payload.filter((p) => p.value != null).map((p) => (
        <div key={p.dataKey} style={{ color: p.color, marginBottom: 1 }}>
          {LINES.find((l) => l.key === p.dataKey)?.label}: {fmtVal(p.value)}
        </div>
      ))}
    </div>
  );
};

const Chart = ({ title, data }) => {
  const allVals = data.flatMap((d) => LINES.map((l) => d[l.key]).filter((v) => v != null && v > 0));
  if (!allVals.length) return null;

  const logNs = data.map(d => d.logN);
  const minX = Math.floor(Math.min(...logNs));
  const maxX = Math.ceil(Math.max(...logNs));
  const xTicks = [];
  for (let i = minX; i <= maxX; i++) xTicks.push(i);

  return (
    <div style={{ marginBottom: 28 }}>
      <h3 style={{ margin: "0 0 6px 0", fontSize: 14, fontWeight: 600, color: "#ddd", textAlign: "center" }}>{title}</h3>
      <ResponsiveContainer width="100%" height={220}>
        <LineChart data={data} margin={{ top: 4, right: 16, left: 8, bottom: 4 }}>
          <CartesianGrid strokeDasharray="3 3" stroke="#2a2a3e" />
          <XAxis dataKey="logN" type="number" domain={[minX, maxX]}
            ticks={xTicks} tickFormatter={fmtN}
            tick={{ fill: "#888", fontSize: 11 }} stroke="#444" />
          <YAxis scale="log" domain={["auto", "auto"]} type="number"
            tickFormatter={fmtVal} tick={{ fill: "#888", fontSize: 10 }}
            stroke="#444" width={52} allowDataOverflow />
          <Tooltip content={<Tip />} />
          {LINES.map((l) => (
            <Line key={l.key} type="monotone" dataKey={l.key} stroke={l.color}
              strokeWidth={l.dash ? 1.5 : 2.5} dot={false}
              strokeDasharray={l.dash || undefined} connectNulls isAnimationActive={false} />
          ))}
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
};

export default function App() {
  const [pattern, setPattern] = useState("random");
  return (
    <div style={{ background: "#0f0f1a", color: "#ddd", minHeight: "100vh", padding: "16px 12px", fontFamily: "system-ui, sans-serif" }}>
      <h2 style={{ margin: "0 0 4px 0", fontSize: 18, fontWeight: 700, textAlign: "center" }}>kntrie Benchmark</h2>
      <p style={{ textAlign: "center", color: "#777", fontSize: 12, margin: "0 0 12px 0" }}>
        Log-log · Per-entry · Lower is better · Solid = i32, Dashed = u64
      </p>

      <div style={{ display: "flex", justifyContent: "center", gap: 12, marginBottom: 16, flexWrap: "wrap" }}>
        {LINES.map((l) => (
          <div key={l.key} style={{ display: "flex", alignItems: "center", gap: 5, fontSize: 11 }}>
            {l.dash ? (
              <svg width={22} height={4}><line x1={0} y1={2} x2={22} y2={2} stroke={l.color} strokeWidth={2} strokeDasharray={l.dash} /></svg>
            ) : (
              <div style={{ width: 22, height: 2.5, background: l.color, borderRadius: 1 }} />
            )}
            <span style={{ color: "#bbb" }}>{l.label}</span>
          </div>
        ))}
      </div>

      <div style={{ display: "flex", justifyContent: "center", gap: 8, marginBottom: 16 }}>
        {PATTERNS.map((p) => (
          <button key={p} onClick={() => setPattern(p)}
            style={{
              padding: "6px 16px", borderRadius: 6, border: "1px solid #444",
              background: pattern === p ? "#3b82f6" : "#1a1a2e",
              color: pattern === p ? "#fff" : "#aaa",
              cursor: "pointer", fontSize: 13, fontWeight: 600,
            }}>
            {p}
          </button>
        ))}
      </div>

      <div style={{ maxWidth: 620, margin: "0 auto" }}>
        {METRICS.map((m) => (
          <Chart key={`${pattern}-${m.suffix}`} title={m.label}
            data={buildData(pattern, m)} />
        ))}
      </div>
    </div>
  );
}
