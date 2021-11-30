// Boost.Geometry
// Unit Test

// Copyright (c) 2018 Adeel Ahmad, Islamabad, Pakistan.

// Contributed and/or modified by Adeel Ahmad, as part of Google Summer of Code 2018 program.

// Use, modification and distribution is subject to the Boost Software License,
// Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_GEOMETRY_TEST_INVERSE_CASES_ANTIPODAL_HPP
#define BOOST_GEOMETRY_TEST_INVERSE_CASES_ANTIPODAL_HPP

#include "inverse_cases.hpp"

struct expected_results_antipodal
{
    coordinates p1;
    coordinates p2;
    expected_result karney;
};

/*
 These values are collected from GeodTest which is associated with GeographicLib:
     https://zenodo.org/record/32156

 The conversion to C++ array format is done using this Python script:
     https://github.com/adl1995/boost-geometry-extra/blob/master/geographiclib-dataset-parse-inverse.py

 Geodesic scale (M12) is absent from the GeodTest dataset, so it is manually generated
 using GeographicLib using this C++ script:
     https://github.com/adl1995/boost-geometry-extra/blob/master/geographicLib-direct-antipodal.cpp
*/
expected_results_antipodal expected_antipodal[] =
{
    {
        { 0, 31.394417440639 }, { 179.615601631202912322, -31.275540610835465807 },
        { 19980218.4055399, 34.266322930672, 145.782701113414306756, 49490.8807994496209, -0.996116451012525883079717914370121434 }
    },{
        { 0, 29.788792273749 }, { 178.569451327813675741, -29.558013672069422725 },
        { 19887224.5407334, 74.302205994192, 106.156240654579267308, 97043.7545600593058, -0.998624031147844926081802441331092268 }
    },{
        { 0, 46.471843094141 }, { 179.083144618009561276, -46.284166405924629853 },
        { 19944337.8863917, 63.693680310665, 116.699978859005570535, 53139.140576552365, -0.997597309645591900917338534782174975 }
    },{
        { 0, 63.016506345929 }, { 179.862869954071637855, -63.02943882703369735 },
        { 20000925.7533636, 153.393656073038, 26.619056019474552953, 12713.9284725111772, -1.00381317792143387457315384381217882 }
    },{
        { 0, 19.796231412719 }, { 179.546498474461283862, -19.470586923091672503 },
        { 19956338.1330537, 28.272934411318, 151.789094611690988249, 87191.1749625132931, -0.997015409027664833985227232915349305 }
    },{
        { 0, 6.373459459035 }, { 179.240009269347556917, -6.204887833274217382 },
        { 19946581.6983394, 56.859050230583, 123.169200847008284851, 53958.8698005263939, -0.999349049081101004077254401636309922 }
    },{
        { 0, 66.380766469414 }, { 179.632633596894388233, -66.27177494016956425 },
        { 19986277.7696849, 38.646950203356, 141.550919825824399405, 22198.215635049214, -0.996949176054954366854587988200364634 }
    },{
        { 0, 16.483421185231 }, { 179.731567273052604726, -16.818424446748042212 },
        { 19962737.9842573, 163.431254767325, 16.598399455529231288, 95318.4104529881431, -1.00272210232979741562076014815829694 }
    },{
        { 0, 4.215702155486 }, { 179.093771177769992874, -4.051917290690976764 },
        { 19932517.393764, 65.543168480886, 114.482669479963380006, 55205.4553703842317, -0.999655858425056553784315838129259646 }
    },{
        { 0, 40.71372085907 }, { 179.404612926861498984, -41.047052242159400671 },
        { 19951133.3595356, 143.672151631634, 36.54002600969304553, 70931.1530155553621, -1.00414169574077272173440178448799998 }
    },{
        { 0, 15.465481491654 }, { 179.020726605204181801, -14.622355549425900341 },
        { 19877383.8879911, 36.289185640976, 143.875673907461159912, 156419.0806764376957, -0.997639074397169589580869342171354219 }
    },{
        { 0, 17.586197343531 }, { 179.722490735835379144, -17.731394230364437075 },
        { 19982280.4639115, 157.929615091529, 22.089021105298661023, 69727.5357849255557, -1.00280451698301242835498214844847098 }
    },{
        { 0, 5.7442768247 }, { 178.85894724576868462, -6.039853564481335581 },
        { 19902873.7431814, 116.146983678305, 63.91482549951374061, 87149.6188944111673, -1.00039332893096744037109147029696032 }
    },{
        { 0, 32.002904282111 }, { 179.744925422107715439, -32.297934520693132807 },
        { 19967670.3104795, 163.052160078191, 17.004175883388454943, 78311.3164829640582, -1.00449903445302446414189034840092063 }
    },{
        { 0, 55.902716926362 }, { 179.300685189522463007, -55.934320218634018206 },
        { 19970525.337607, 98.927641063414, 81.374264168520557301, 23554.0093185709067, -1.00072788779083454713259015989024192 }
    },{
        { 0, 22.69939784398 }, { 179.294173474584020749, -22.654875407651067149 },
        { 19959286.1903172, 74.253870776761, 105.811588890213155275, 22369.7179951557679, -0.998972181419003457669703038845909759 }
    },{
        { 0, 41.312328471121 }, { 179.817186837717804928, -40.954523601529804886 },
        { 19962690.5721867, 11.277616109847, 168.784288786443902199, 77252.6121237260201, -0.994825151471527391322524636052548885 }
    },{
        { 0, 27.927415327453 }, { 179.636508875679110143, -27.607314264234172721 },
        { 19961296.8828333, 23.166421459647, 156.905194492817275222, 83096.5801709291101, -0.995959692767656723511038308060960844 }
    },{
        { 0, 41.567228741451 }, { 179.931812964300204608, -42.103039532074194347 },
        { 19944253.4454809, 176.66609526064, 3.361859685835349219, 96859.08180779197, -1.00513607140487626345759508694754913 }
    },{
        { 0, 37.384208978567 }, { 179.225180174670992261, -36.916085670712060029 },
        { 19928705.5911445, 39.072534864532, 141.212743814390850106, 92667.7834060578402, -0.995955516859159284415170532156480476 }
    },{
        { 0, 59.011868682852 }, { 179.424923485514312807, -58.82705468054708336 },
        { 19970442.3788306, 44.970301291063, 135.333817989802309531, 38071.1136293083857, -0.996658942892707400140750451100757346 }
    },{
        { 0, 35.515406087737 }, { 179.50369572149476218, -35.119747127350258822 },
        { 19948918.9139751, 28.528972431952, 151.622257906284404073, 84564.0387217601751, -0.995562861799169418475230486365035176 }
    },{
        { 0, 58.170252463184 }, { 179.254737571455023977, -58.372261836268550805 },
        { 19961407.0813807, 128.021116291844, 52.399129705193347143, 43715.3070711393309, -1.00285273713280753682397516968194395 }
    },{
        { 0, 34.012183807959 }, { 179.83713352180447672, -34.29640782899529639 },
        { 19970955.843065, 168.944519134772, 11.093048811826875835, 76493.5814538538151, -1.0047652354558671561335359001532197 }
    },{
        { 0, 45.510762948553 }, { 178.981682578823726535, -45.582753595227824235 },
        { 19940248.3450143, 99.886784003837, 80.542330522982505877, 48555.1946627894972, -1.00083807750906350619857221317943186 }
    },{
        { 0, 4.19841765451 }, { 179.398024428225540172, -4.198416896099783242 },
        { 19970496.5132933, 89.561550657928, 90.438456568689151881, 14.8790480103109, -0.999994104810285944218151144013972953 }
    },{
        { 0, 40.890119148103 }, { 179.6557148951668192, -41.553556264538302258 },
        { 19926563.5817492, 165.437641169967, 14.713597527941311478, 111805.7305227545923, -1.00492294933406567380984597548376769 }
    },{
        { 0, 28.096672787686 }, { 178.606868012231657724, -28.472055035513955205 },
        { 19883901.8482359, 115.174366374632, 65.257367020445564176, 107880.4353518862363, -1.00170803073331593502359737613005564 }
    },{
        { 0, 6.50572154271 }, { 178.926013840891647541, -6.411745140559297675 },
        { 19917276.4101551, 79.069492719523, 100.985091481519557845, 57073.3242952680707, -0.999736666933808471036115861352300271 }
    },{
        { 0, .468835109567 }, { 178.325942223692180692, -.281751687044281805 },
        { 19849380.7342734, 80.234636214474, 99.77243368342786593, 123845.4568822078908, -0.999801437209140719808431185811059549 }
    },{
        { 0, 1.682746325049 }, { 179.717131561406935483, -.677647430701204515 },
        { 19890026.0274781, 10.076182752451, 169.927471515299313238, 177917.2104306563981, -0.999538055691262194990542866435134783 }
    },{
        { 0, 10.711305126218 }, { 179.874050163405229937, -10.349315378531556046 },
        { 19962987.2134077, 7.528253696796, 172.480576051850009046, 104175.1095378254456, -0.998071853755238880268052525934763253 }
    },{
        { 0, 53.374321544652 }, { 179.729445806011012057, -53.196257519024042184 },
        { 19980478.1457438, 23.324715976877, 156.777734080146664812, 41907.8869272231053, -0.995333596277707566279957518418086693 }
    },{
        { 0, 39.680221664519 }, { 179.87506206720154785, -39.256187213040660911 },
        { 19956191.7841809, 7.075406493429, 172.967624741991546131, 86943.8110669895148, -0.994801087909667924868983845954062417 }
    },{
        { 0, 1.377666714083 }, { 178.994542525209058878, -1.415358715570225495 },
        { 19925401.4931301, 95.29199069739, 84.7178724483824156, 45800.9140624827059, -0.99999803170512457928253979844157584 }
    },{
        { 0, 48.751426624188 }, { 179.661697715070846977, -48.688146707479475147 },
        { 19988599.1160495, 40.252328570137, 139.808452951157199824, 26322.3790862461568, -0.995999245724129789181233718409202993 }
    },{
        { 0, 59.443039048494 }, { 179.247605418616998285, -59.454371825393424121 },
        { 19969935.9534732, 93.052184108221, 87.331416513795326158, 25342.4691896499534, -1.00020727848897084122370415570912883 }
    },{
        { 0, 4.122408476235 }, { 179.749430572914989772, -4.689124208743755363 },
        { 19938291.6332293, 167.73479753304, 12.274635577599782826, 127855.6475863583497, -1.00068600902837667732114823593292385 }
    },{
        { 0, 46.422470082432 }, { 178.857408435141563774, -46.390934261324541952 },
        { 19931980.7029341, 86.67365350297, 93.852683224054943377, 56114.680046867064, -0.999607096116300386512421027873642743 }
    },{
        { 0, 32.614423729024 }, { 179.460593512880455451, -32.01874745886238612 },
        { 19926887.3785175, 24.943814520557, 155.229917137448282531, 112355.3319340873104, -0.995562150676871926435751447570510209 }
    },{
        { 0, 3.242895277973 }, { 179.556428318080663113, -3.001106476068264917 },
        { 19964490.4789049, 30.247458779683, 149.760178923092147784, 80929.0418317066044, -0.999474184270344845337774586369050667 }
    },{
        { 0, 6.29069210113 }, { 178.556859259685624933, -6.354208910915346725 },
        { 19877160.8505733, 94.34299459284, 85.750059038253282986, 94127.1566760840083, -0.999976397350904933070125935046235099 }
    },{
        { 0, 18.232086569498 }, { 179.658073278238477245, -18.87394850776853555 },
        { 19927978.7462175, 164.41905055334, 15.640779355822506503, 129771.1882449660559, -1.00293460439063886191490837518358603 }
    },{
        { 0, 12.049849333181 }, { 179.761046682699610657, -11.201990279782499264 },
        { 19908004.4552909, 9.418096768309, 170.610608272305604585, 157761.5040571466343, -0.997761474497510958414636661473196 }
    },{
        { 0, 40.289465276136 }, { 179.644208494155329095, -40.370034926441385999 },
        { 19985674.936106, 143.092606818963, 36.958610382613096419, 36200.8933724688593, -1.00414965876091266672176516294712201 }
    },{
        { 0, 2.197784650379 }, { 179.961199531084784854, -1.353440827124394777 },
        { 19910509.7517973, 1.542117609437, 178.458582198505846426, 160403.6285079348996, -0.999488724639301051588802238256903365 }
    },{
        { 0, 1.966575272177 }, { 179.699817324905962184, -3.101125282483752618 },
        { 19875595.6267266, 170.112968791865, 9.89572776349855838, 192355.7206665719908, -1.00015463589804554089823795948177576 }
    },{
        { 0, 25.078832492684 }, { 178.600804840925824646, -24.897833702325682511 },
        { 19887997.7953866, 77.264585323781, 103.101167809583406892, 92442.9124509225839, -0.998981189838600847075156252685701475 }
    },{
        { 0, 31.740361941314 }, { 179.553485210731879874, -31.909206787477701871 },
        { 19972325.3556069, 143.930820896999, 36.145242998351638503, 54883.4113710054145, -1.00379461628115951299378139083273709 }
    },{
        { 0, .05479250563 }, { 178.822647462220726609, .836079031223269324 },
        { 19858049.4780499, 41.349430623518, 138.645259065012502544, 169078.442370111714, -0.9997793696948588104689292777038645 }
    },{
        { 0, 36.685139871608 }, { 179.366667224014334712, -36.6833040833258687 },
        { 19968965.6773632, 89.167975517493, 90.921025521408327068, 13327.2156799476918, -0.999916537946348604748436628142371774 }
    },{
        { 0, 3.451199399671 }, { 179.107509334399258305, -3.459003521120242021 },
        { 19938203.3838544, 91.541212417048, 88.476282464773035164, 32316.1747698810781, -1.00000397484395819880376166111091152 }
    },{
        { 0, 27.692898794247 }, { 178.512356615673144314, -27.666009301228316555 },
        { 19883493.6699045, 88.406440883665, 92.036345087713397961, 94128.7880896190836, -0.999736458322951659916100197733612731 }
    },{
        { 0, 17.363238291869 }, { 179.567921315455829491, -17.288872648596950413 },
        { 19980749.7638027, 39.697196316589, 140.321938237586060826, 46975.9359427664379, -0.997687691981715030209443284547887743 }
    },{
        { 0, 37.006775102539 }, { 179.191103068859169842, -37.156365616364686838 },
        { 19949309.9180043, 116.455543532607, 63.771817992036617793, 45856.1961421018701, -1.00221962858918423044940482213860378 }
    },{
        { 0, 45.572883540957 }, { 179.224707765088686272, -45.94675931323086696 },
        { 19940027.8586414, 137.627256708444, 42.723991162977357301, 74208.4359612889496, -1.00380887786447159371050474874209613 }
    },{
        { 0, 43.63393981955 }, { 178.878236417027994157, -43.642335115130514773 },
        { 19931045.2914508, 91.203625101465, 89.268780774643462256, 55253.5406349861764, -1.00002974153150514524668324156664312 }
    },{
        { 0, 38.4995307019 }, { 179.143856004445269342, -39.042223438550921467 },
        { 19918391.2222193, 141.232864609445, 39.117947060740562295, 102217.2563106863077, -1.00388164115732947401227193040540442 }
    },{
        { 0, 27.55015339382 }, { 179.596220103573824099, -27.587412128122249651 },
        { 19986004.7358853, 137.025135713548, 42.992898351962011956, 33938.7346646670654, -1.00316044390281167153489150223322213 }
    },{
        { 0, 1.54507498314 }, { 179.567115633151308577, -1.448861185025252004 },
        { 19978593.3191777, 36.816106412092, 143.185763012309022403, 56320.5800276739168, -0.999770499462467210349814195069484413 }
    },{
        { 0, 45.217063644222 }, { 179.807382581661125, -45.086424050571516283 },
        { 19987042.0782465, 18.114645812265, 161.928120141429818658, 45544.2915061261936, -0.994974179414854997816064496873877943 }
    },{
        { 0, 13.473522450751 }, { 179.726941062277208626, -13.570372758027936877 },
        { 19987364.078382, 156.839609002403, 23.170293747820406391, 65329.9068132034472, -1.00219093189506569530067281448282301 }
    },{
        { 0, 6.287741997374 }, { 179.071252372259552052, -6.743450924917895817 },
        { 19912159.8245954, 132.954797451112, 47.100789519677419746, 104772.4027498097375, -1.00071252411103017720961361192166805 }
    },{
        { 0, 7.639709001531 }, { 179.616156296978583335, -7.48702643786017917 },
        { 19976374.3699535, 29.731916588299, 150.279582966919438164, 69224.6591757209539, -0.998789792086741234911073661351110786 }
    },{
        { 0, 5.893688050348 }, { 179.586212000450856399, -4.888408917114795625 },
        { 19886907.2520668, 14.653438882877, 165.371181401863458848, 177183.5330818593022, -0.998794647031120752522781458537792787 }
    },{
        { 0, 61.997076235476 }, { 179.605779116829636081, -62.19593758437129915 },
        { 19976288.2901729, 149.562797049254, 30.65850204223272625, 36696.2853801462176, -1.00373071432437144245852778112748638 }
    },{
        { 0, 50.507637741656 }, { 179.893569206021038536, -50.721890799900161112 },
        { 19979542.5263293, 171.564028344478, 8.4746613464253591, 50644.5234828162697, -1.00508881632281776852266830246662721 }
    },{
        { 0, 7.484475238477 }, { 178.638400003000590878, -6.926155588124333461 },
        { 19867425.2906303, 57.020570370985, 123.087267812322270238, 132929.2775641349633, -0.999097042677338120775232255255104974 }
    },{
        { 0, 56.851165323215 }, { 179.587046628550073045, -56.875248360744638525 },
        { 19988235.9960515, 112.345749045605, 67.744017057185404441, 9971.0934553515518, -1.00182859249871403228837607457535341 }
    },{
        { 0, 10.692273150738 }, { 178.709520715733071393, -10.851727623036704339 },
        { 19893210.3050033, 102.824601316946, 77.308514969817191459, 83032.7122948051111, -1.00034345584508432835946223349310458 }
    },{
        { 0, 46.694739303788 }, { 179.926838145841924189, -46.948618153686522669 },
        { 19975447.9283188, 174.663684259477, 5.361568174833475454, 59614.5876209460645, -1.00520484875201732144489596976200119 }
    },{
        { 0, 15.804386137005 }, { 178.367587635209819128, -15.522042847777054984 },
        { 19855850.8800526, 74.932089158884, 105.357235560913450667, 123350.4326645237628, -0.999091578546475345135036150168161839 }
    },{
        { 0, 4.371450175299 }, { 179.780887420199549421, -4.566109732313098407 },
        { 19979071.1035552, 164.163592252794, 15.840695025950408814, 84137.2115482558728, -1.00076323969894742660358133434783667 }
    },{
        { 0, 30.894388279688 }, { 179.375426183521944524, -30.871308884744172663 },
        { 19968681.8321577, 77.35154610481, 102.709506078439532936, 14048.0277985734058, -0.998975176336422854284080585784977302 }
    },{
        { 0, 9.541166838639 }, { 178.432934555386452839, -10.09982228112793472 },
        { 19848553.7844137, 118.441353539081, 61.736686215549403663, 144831.1911566651614, -1.00060548620110489892454097571317106 }
    },{
        { 0, 8.489292700054 }, { 179.906698338023119097, -8.559237750032113623 },
        { 19995477.1669578, 171.963952699866, 8.037517851139094467, 72192.60793572974, -1.00152068486306466965629624610301107 }
    },{
        { 0, 19.562401114224 }, { 178.838724116996037606, -20.05038360490599475 },
        { 19893208.1788508, 126.362762598128, 53.875560227496658204, 112181.7524188837615, -1.00185202668802775249901060306001455 }
    },{
        { 0, 42.260350252749 }, { 179.807860448877064601, -42.79985897702184353 },
        { 19942715.0054774, 170.703419847646, 9.377654670896439828, 96336.3477142010769, -1.00508642406443549077721399953588843 }
    },{
        { 0, 24.511403144656 }, { 178.957598444862223515, -24.616808725039883945 },
        { 19924809.5184876, 102.913211410163, 77.297538210434837096, 55403.453072179318, -1.0008408309188838725134473861544393 }
    },{
        { 0, 20.844284170708 }, { 179.069258863637226633, -20.321320573298341477 },
        { 19909084.6340808, 44.172784008084, 136.01627115731728436, 111009.0987238994608, -0.997389183621778974142557672166731209 }
    },{
        { 0, 2.426010809098 }, { 178.236397468862000784, -2.513715200833756776 },
        { 19840940.6924189, 94.315194952561, 85.734896842737189557, 130002.6104886615638, -0.999825249844991659209370027383556589 }
    },{
        { 0, 6.600682554664 }, { 179.646475458013797028, -7.699164822656561787 },
        { 19878412.28273, 168.167678684515, 11.861035812918738552, 187426.3958525886692, -1.00098284856064978498579876031726599 }
    },{
        { 0, 23.372339802326 }, { 179.499422665106094027, -24.239465200482591299 },
        { 19899498.4582543, 161.197647943542, 18.932355367478826536, 151863.2545535951091, -1.00347666868431395492677893344080076 }
    },{
        { 0, 16.194668264095 }, { 179.115193814080201851, -17.129419031459576897 },
        { 19874825.6683239, 148.942349959054, 31.225656401221968078, 166033.3161394594622, -1.00222032222233647935638600756647065 }
    },{
        { 0, 1.528726471528 }, { 178.791047180477802091, -1.282203000582034597 },
        { 19897803.9939987, 69.212891442493, 110.802928803578167132, 85252.8333849204133, -0.999827144228156883265512533398577943 }
    },{
        { 0, 6.297249676078 }, { 178.623258703845895437, -5.709470001196540278 },
        { 19864042.0495193, 56.274639904925, 123.817184177744186806, 137475.1283083659258, -0.999190450178399580671850799262756482 }
    },{
        { 0, 17.393540327984 }, { 179.330156510680163326, -17.431100690958209424 },
        { 19962624.6302607, 107.855062015266, 72.181322855288535245, 19320.5501845044839, -1.00091841779689127989172447996679693 }
    },{
        { 0, 46.284685151236 }, { 179.852534804091121255, -46.176234945675219984 },
        { 19990422.3478916, 14.758013867151, 165.271681964991897184, 42614.1796365710104, -0.994894592261839960656288894824683666 }
    },{
        { 0, 14.924320176299 }, { 179.195663739713760883, -14.125476432252858442 },
        { 19891861.8615337, 31.446544793174, 148.678916887199611191, 149419.6596309045804, -0.997620142585332936313591289945179597 }
    },{
        { 0, 23.668824656069 }, { 179.409875478773990359, -24.107855233601412399 },
        { 19938736.4442268, 148.091483667618, 32.02919257641173958, 97771.7687385830819, -1.00323262872000595891108787327539176 }
    },{
        { 0, 46.986276695896 }, { 179.92040916864362177, -47.301644191214905832 },
        { 19968596.0414782, 174.796708941456, 5.234240076649939638, 66113.7417494369769, -1.00519095452608087093437916337279603 }
    },{
        { 0, 65.946144289524 }, { 179.808282612725835525, -65.871840130833632868 },
        { 19993734.5109736, 25.375428509648, 154.703163938350061652, 18355.2254271672769, -0.996436935914610577569305860379245132 }
    },{
        { 0, 10.950298933293 }, { 179.624609619829763098, -10.787771536605316781 },
        { 19975919.5586889, 28.779018914489, 151.238005588662201946, 70291.1998404303581, -0.998272071834115148902810688014142215 }
    },{
        { 0, 13.609869340778 }, { 179.035623147420893383, -14.023624108675206222 },
        { 19913213.8514358, 129.616021271129, 50.506400999466711623, 97596.7664002074776, -1.00146664642314031645753402699483559 }
    },{
        { 0, 48.701427557433 }, { 179.385565054218238481, -48.735316652259656533 },
        { 19972955.2699173, 102.875149183407, 77.294384444682547869, 18461.7742226227697, -1.00114676855429074464609584538266063 }
    },{
        { 0, 31.519172055785 }, { 179.555251675378549409, -31.140142027808697534 },
        { 19952318.3772514, 26.247105619999, 153.865822276646938125, 86354.7117605101002, -0.995739948399825047786748655198607594 }
    },{
        { 0, 31.863784754278 }, { 179.722489476483407524, -31.826935359797657785 },
        { 19993324.8682601, 29.572313410211, 150.440607907359037187, 41427.6181613499234, -0.995888009001147267440501309465616941 }
    },{
        { 0, 76.434608546092 }, { 179.918287057674124459, -76.48787937532808951 },
        { 19997750.023578, 167.428385412814, 12.621032110142724567, 9619.5267710862108, -1.00233963893091582164629471662919968 }
    },{
        { 0, 73.114273316483 }, { 179.576736605988553624, -73.098788070892914568 },
        { 19992866.6147806, 78.154765899661, 102.085693546950923465, 8580.6475692800946, -0.999384143308475469957841141876997426 }
    },{
        { 0, 1.125639056292 }, { 178.426819580880619395, -.694775021853292564 },
        { 19852573.5442848, 67.184842289382, 112.831314850896246589, 132932.8743502563937, -0.999732957962833457266071945923613384 }
    }
};

size_t const expected_size_antipodal = sizeof(expected_antipodal) / sizeof(expected_results_antipodal);

#endif // BOOST_GEOMETRY_TEST_INVERSE_CASES_ANTIPODAL_HPP
