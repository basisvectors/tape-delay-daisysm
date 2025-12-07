// TAPE SYSTEM GENEXPR

// FUNCTIONS STAGE

// smoothing
smpsmooth(v, s) {				// s = smoothing amount (0.9995)
	History smp(0);
	y = mix(v, smp, s);
	smp = y;
	return y;
}
/*
cpsm(x, a) { 					// a = smoothing amount (0.999)
	History z(0);
	b = pow(a, (1 / (samplerate / 44100)));	// compensate	// expensive pow bad
	c = 1 - b;
	z = (x * c) + (z * b);
	return z;
}
*/
cpsm(a, f) {					// f = smoothing amount, strange range, try 0.999
	History s(0);
	x = (1 - f) * (44100 / samplerate);		// compensate SR (x must not be > 1, i.e. SR must not be < 44.1kHz, no clip here risky?)
	y = ((a - s) * x) + s;
	s = y;
	return y;
}

rsmooth(x, s) {					// s = time to drop 6dB (in seconds)
	History z(0);
//	ad = 0.693147 / maximum((s * samplerate), 1);	// 'maximum' check not actually required?
	ad = 0.693147 / (s * samplerate);
	y = ((x - z) * ad) + z;
	z = y;
	return y;
}
/*
// experiment... ...too expensive :(
lsp(x, c, l) {								// value / coefficient / previous
	return ((1 - abs(c)) * x) + (c * l);
}
// ! expensive !
lsmooth(v, lp0, th) {						// destination value / time (in seconds) / threshold (use 0.0001)
	History coef(0);
	History lp1(0), lp2(0), lp3(0);
	if (change(lp0)) {
		if (lp0 <= 0) {
			coef = 0;
		} else {
			coef = exp(log(th) / (lp0 * samplerate));
		}
	}
	lp1 = lsp(v,	coef,	lp1);
	lp2 = lsp(lp1,	coef,	lp2);
	lp3 = lsp(lp2,	coef,	lp3);
	return lp3;
}
*/
// approximations
expA(x) {						// exp approximation	// actually approx exp(2x)
	x = x * 2;
	x = 0.999996 + (0.031261316 + (0.00048274797 + 0.000006 * x) * x) * x;
	x *= x; x *= x; x *= x; x *= x;
	return x * x;
}

tnA(x) {						// tan approximation
	x2 = (x * x);
	x3 = (x2 * x);
	x5 = (x2 * x3);
	return ((x5 * 0.133333) + (x3 * 0.333333)) + x;
}

cosApp01(a) {					// cosine approximation
	p = wrap(a, 0, 1) - 0.75;
	pa = abs(p);
	cl = ((pa - 0.5) * (pa - 0.924933)) * (pa + 0.424933);
	cr = (((pa + 1.05802) * pa) + 0.436501) * ((pa * (pa - 2.05802)) + 1.21551);
	return p * ((cl * cr) * 60.252201);
}

sinApp01(a) {					// sine approximation (wlfo)
//	return cosApp01(a - 0.25);	// one line declaration causes compile error on 6.1.8 - weird
	b = a - 0.25;
	s = cosApp01(b);
	return s;
}

// non-linearities
tnhLam(x) {						// tanh Lambert approximation
	x2 = x * x;
	a = (((x2 + 378) * x2 + 17325) * x2 + 135135) * x;
	b = ((28 * x2 + 3150) * x2 + 62370) * x2 + 135135;
	return clamp((a / b), -1, 1);// should not need to clamp here ?
}

tnhb(x, m) {					// modifiable tanh approximation (must not be using exp approximation)
	exb = exp(m * x);
	return (2 / (1 + exb)) - 1;	// tanh if m == -2, 'tape' (!) if m = -5
}

polysat(x) {					// polynomial saturation
	x2 = x * x;
	x3 = x * x2;
	p531 = (x + (x3 * -0.18963)) + ((x3 * x2) * 0.016182);
	pn = -1.875 > x ? -1 : p531;
	y = x > 1.875 ? 1 : pn;
	return y * 0.999995;		// polysat needs final attenuation for inputs > +5dB (0.999996 but...)
}

cnl(x) {						// cubic non-linearity (quasi josIII)
	x = clip(x, -1, 1);
	return x * (1 - 0.333333 * x * x);	// 0.33333333
}

parsat(x0, a0, /*c0*/c1) {		// parabolic saturation
	x1 = clip(x0, -1, 1);
//	c1 = maximum(c0, 0);				// branch not needed
	c2 = c1 + c1;
	x2 = clip((x1 * a0), (c2 * -1), c2);
	y = x2 * (1 - (abs(x2) * (0.25 / c1)));
	return y;
}

// modifiers
tnhbMod(mod) {
	m0 = minimum(mod, 1);		// we only need branch 'cos 'modifier' can go > 1; shit
	m1 = scale(m0, 1, 0, -5.067268, -1.098611, 0.38103);	// approx 0.5 = approx tanh
	return m1;
}

parsatMap(ind) {	// x		// who needs luts ?!
//	ind = clip(x, 0, 2);
	y = 0;
	if (ind == 1) {
		y = 0.6;
	} else if (ind == 2) {
		y = 1.5;
	} else {	// (ind == 0)
		y = 0.5;
	}
	return y;
}

parsatMod(mod) {
	feeder = mod * 2;
	ate = trunc(feeder);
	ion = fract(feeder);
	map0 = parsatMap(ate);
	map1 = parsatMap(ate + 1);
	sender = interp(ion, map0, map1, mode="cosine");		// lookup
	return sender;
}

// non-linearity selector ('quality' == 2)
sat(nl, x, mod) {
	dcba = 0;
	y = 0;
	if (nl == 1) {				// polynomial			static
		dcba = 0.976322;		// polysat requires attenuation for dcblock() + 15dB
		y = polysat(x);
	} else if (nl == 2) {		// cubic				static
		dcba = 1;
//		x = clip(x, -1, 1);		// cnl requires clip
		y = cnl(x);
	} else if (nl == 3) {		// parabolic			+ modifier (for in/out stages)
		dcba = 1;
//		x = clip(x, -1, 1);		// parsat requires clip
		m = parsatMod(mod);
		a = 1;
		y = parsat(x, a, m);
	} else { // nl == 0			// s-curve (default)	+ modifier (for s-curve)
		dcba = 0.976047;		// tnhb requires attenuation for dcblock() + 15dB
		m = tnhbMod(mod);
		y = tnhb(x, m);
	}
	y = dcblock(y);
	return y * dcba;			// all dcblocks (& gain compensation) happen here
}

// passive saturation
simpSat(xin) {
	x = clip(xin, -1, 1);		// without clip it is a waveshaper
	return 0.5 * x * (3 - (x * x));
}

// static limit, processes at 1, limits at 4 (this function is actually a bit pointless ?)
softStatic(x) {
	if (x > 1)
		x = (1 - 4 / (x + 3)) * 4 + 1;
	else if (x < -1)
		x = (1 + 4 / (x - 3)) * -4 - 1;
	else
		x = x;
	return x;
}

// like MSP [slide~]
p_SlideLite(x, u, d) {							// input, up in samples, down in samples
	History current(0);
	s = x - current;
	us = s * (1 / (maximum(abs(u), 1)));
	ds = s * (1 / (maximum(abs(d), 1)));
	y = current + ((x > current) ? us : ds);
	current = y;
	return y;
}

// VU-style 'analogue'-ish envelope follower, with quirky optional 2-decay system
p_Env(x, r, a, d, m, t) {						// r, a, d, m must be clipped 0..1
	r1 = r + 1;
	f = clip((abs(x) * (r1 * r1)), 0, 1);		// abs could be average, & no pow here
	au = mstosamps(expA(a * 7));				// approx...
	dd = mstosamps(expA(d * 7));
	env1 = p_SlideLite(f, au, dd);
	env2 = 0;
	if (t > 0) {								// only compute if called
		dm = dd * (expA(m * 5) + 2);
		env2 = p_SlideLite(f, au, dm);
	} else {
		env2 = 0;								// env2 = env1 ?
	}
	return env1, env2;
}
/*
// basic cheap(er) envelope follower - used to use in Magnetic version 1 for quality == 1
scEF(f, t, i, c) {					// 'f' = audio to follow, 't' = abs or rms, 'i' = interpolation, c = compression
	History iir(0);
	follow = 0;						// init
	if (t == 1) {
		follow = f * f;
	} else {	// t == 0
		follow = abs(f);
	}
	env0 = mix(follow, iir, i);
	env1 = (clamp(env0, 0, (1 - (c * 0.5))) * (c + 1));
	iir = env0;
	return clip(env1, 0, 1);		// envelope of 'f'
}
*/
// trapezoidal loop ducking, no exponents now
hTrap(ph, lo, hi, up, down) {
	phw = wrap(ph, 0, 1);
	ucl = clip(up, 0, 1);
	dcl = clip(down, ucl, 1);
	phwucl = phw < ucl;
	phwdcl = phw > dcl;
	hml = hi - lo;
	ds = lo + ((hml * (1 - ((phw - dcl) / (1 - dcl)))));
	us = lo + ((hml * (phw / ucl)));
	r1 = phwdcl ? ds : hi;
	r2 = phwucl ? us : r1;
	return r2;
}

// Mode Selector conflagration
plus2A(a, b) {
	return (a + b) * 0.6;
}

plus2B(a, b) {
	return (a + b) * 0.4;
}

plus2C(a, b) {
	return (a * 0.435) + (b * 0.665);
}

plus2D(a, b) {
	return (a + b) * 0.55;
}

hscale(typ, hld) {					// rescale 'conflagration' if in 'hold' mode
	hs = 0;
	if (hld > 0) {					// only rescale if 'hold' engaged
		if (typ == 1) {				// 1 == plus2A
			hs = 0.833333;
		} else if (typ == 2) {		// 2 == B
			hs = 1.25;
		} else if (typ == 3) {		// 3 == C || D
			hs = 0.909091;
		} else {	// type == 0	// just in case
			hs = 1;
		}
	} else {						// if hold == 0 then ignore
		hs = 1;
	}
	return hs;
}

// trap filters
eAllPoleLPHP6(type, x, cutoff) {			// 6dB onepole
	History y0(0);
	f = clip(sin(cutoff * twopi / samplerate), 0.00001, 0.99999);
	lp = mix(y0, x, f);
	y0 = lp;
	y1 = 0;									// (could be simultaneous outs, but not needed here)
	if (type == 1) {
//		y1 = x - lp;						// highpass
		y1 = lp - x;						// ! intentionally 'wrong' highpass !
	} else {	// type == 0
		y1 = lp;							// lowpass
	}
	return y1;
}

ePoleZeroLPHP(type, x, cutoff) {			// 6dB pole / zero
	History R(0);
	fc = (pi * (maximum((minimum(cutoff, (samplerate * 0.5))), 1))) / samplerate;
	a0 = minimum(((2 * sin(fc)) / (cos(fc) + sin(fc))), 0.999999);	// expensive, need to sort out
	a1 = 1 - (a0 * 2);
	w = x * a0;
	lp = R + w;
	R = w + (lp * a1);
	y = 0;									// (could be simultaneous outs, but not needed here)
	if (type == 1) {
		y = x - lp;							// highpass
	} else {	// type == 0
		y = lp;								// lowpass
	}
	return y;
}

SallenAndKey(type, v0, cutoff, res) {					// 12dB trapezoid integration, plain (no nl)
	// Clear
	History ic1eq(0), ic2eq(0);
	// Set
	g = tnA(PI * (cutoff / samplerate));				// approx
	k = 2 * res;
	gp1 = 1 + g;
	a0 = 1 / ((gp1 * gp1) - (g * k));					// & no pow here
	a1 = k * a0;
	a2 = gp1 * a0;
	a3 = g * a2;
	a4 = 1 / gp1;										// a4 = g*a0;
	a5 = g * a4;
	// Tick
	v1 = (a1 * ic2eq) + (a2 * ic1eq) + (a3 * v0);		// in1 = v0
	v2 = (a4 * ic2eq) + (a5 * v1);						// v2 = a2*ic2eq + a4*ic1eq + a5*v0;
	// Update
	ic1eq = (2 * (v1 - (k * v2))) - ic1eq;
	ic2eq = (2 * v2) - ic2eq;
	// Out (could be simultaneous, but not needed here)
	y = 0;
	if (type == 1) {
		y = v0 - v2;									// highpass hack
	} else if (type == 2) {
		y = v1 - v2;									// bandpass hack
	} else {	// type == 0
		y = v2;											// lowpass s&k
	}
	return y;
}

LoresHipass(x, cf, q) {						// basic lowpass resonant like MSP [lores~] but flipped as a dirty highpass
	History ya(0), yb(0);
	frad = cos(cf * twopi / samplerate);
	res = 0.882497 * exp(q * 0.125);
	scl = (frad * res) * -2;
	r2 = res * res;
	scin = x * ((scl + r2) + 1);
	oput = scin - ((scl * ya) + (r2 * yb));
	yb = ya;
	ya = oput;
	return x - oput;	// flip
}

// comp + hysterisis
feederCompression(x, h0, h1) {				// expand (!) trap filter ouputs
	History curr(0);
	c = x - curr;
	s = (x > curr) ? (c * h0) : (c * h1);	// h1 = HYSTERISIS here (decay) / h0 = wrongness (attack)
	b = fixdenorm(curr + s);
	y = 1 - (b * 0.5);
	curr = b;
	return y;								// for multiplier
}

// DECLARE STAGE

Buffer Tape;									// alias to parent, 4 seconds 2 channels * 2 (double buffer);
												// master 'Tape' buffer as MSP buffer~ rather than
												// gen~ Data so that size can be dynamically
												// managed at runtime; shame because we lose our
												// 64-bits resolution this way...

Data fatPete(16384, 4);							// local non-linearities tables (for 'quality' == 0 || 1)

History initlut(1);								// initialise fatPete LUT on load (1)
History globalaccum(0);							// global phase reference (used to be [accum(1)] no reset !
History syncHoldMaster(1);						// sync not smooth play loop for Hold mode
History syncRevMaster(1);						// sync not smooth Delay Time for reverse modes
History head1(0);								// tapehead1 phase loopback
History fbL(0), fbR(0);							// global feedbacks L&R

// munge heads / flutter
Delay mungephase(909, 2, feedback=0, interp="linear");		// (incredibly serious amount of samples)

// Tape Heads 2
Delay head2L(samplerate, 1, feedback=0, interp="cubic");	// all cubic not spline, we need shape for high frequency response...
Delay head2R(samplerate, 1, feedback=0, interp="cubic");	// ...maybe even try 'cosine' instead ?

// Tape Heads 3
Delay head3L(samplerate, 1, feedback=0, interp="cubic");
Delay head3R(samplerate, 1, feedback=0, interp="cubic");

Param intensity(0.615385, min=0, max=1);		// master feedback
// flutter, filter resonance, tape hysterisis and decay all modified slightly by the 'character' slider
Param character(0.25, min=0, max=1);			// master style
Param direction(0, min=0, max=1);				// 0 = forwards (default), 1 = reverse
Param hysterisis(0.0002, min=0.000001, max=0.71);// (factor of intensity, character, flutter & envelope)
Param quality(0, min=0, max=2);					// global 'quality' settings (mainly for 'processing' 1, 2x, 4x)
Param nonlin(0, min=0, max=3);					// select saturation type (different non-linearities to tape)

// attenuate hold 'unity' when delay values very small
Param holdatten(0.998531, min=0.994, max=1);	// repeat dial 01 < 0.343212 = 0.995..0.999999, > 0.343212 = 0.999999
Param wow(0.007014, min=0, max=1);				// % 0..1 (same slider as 'Flutter' but rescaled, 0..0.995 depending on 'character' externally)
//Param reversestyle(1, min=0, max=2);			// reverse... 0 == play heads, 1 == delay loop and step writehead, 2 == '1' but playhead
Param reversestyle(1, min=0, max=1);			// reverse... 0 == play heads, 1 == delay loop and step writehead

Param rvrbRoute(0, min=0, max=2);				// if 'mode'=='12' && 'reverb routing'=='post', mirror inputs to outputs
Param topology(0, min=0, max=3);				// various filter setups, all lp's followed by hp's
// attenuate frequency response with delay size for '201', only when trap filters 'topology 0' selected
Param freqatten(1, min=0.25, max=2);			// calculated externally 0..1: expr ((1 - (in1 * in1)) * 0.534) + 0.501
// ^ multiplies set frequency of lowpass inside feedback loop from 1 .. 0.5; models analogue delay behaviour of
// longer delay times causing high frequency response to lessen

Param InitialDelay(250, min=10, max=1000);		// MASTER DELAY	// 10..1000
Param Mode(5, min=1, max=12);					// MASTER MODE	// 1..12
Param Hold(0, min=0, max=1);					// MASTER HOLD	// 0/1

//Param global_4n_in_samples(24000);			// for sync (floor)

Param MUTE(0, min=0, max=1);					// GLOBAL MUTE (for Mode == 12)
M = int(MUTE);

H = sah(Hold, syncHoldMaster, 0.5, init=1);		// sync Hold changes

dir			= int(direction);
q			= int(quality);
nl			= int(nonlin);

//cw			= 0;//character > 0.75;					// 'compensate'
//ew			= 0;//int(boostwow);					// 'envelope2'
cw			= nl > 1;
ew			= wow > 0.5;
revstyle	= int(reversestyle);
rvRo		= int(rvrbRoute);
tplgy		= int(topology);

fatso		= dim(fatPete);
tdim		= dim(Tape);
halftdim	= tdim / 2;

// CREATE LOOKUP TABLES STAGE

// for 'quality' == 0 || 1, fatPete,
// ('quality' == 2 is internal realtime computation 'sat' function)

// only (re)initialise 'fatPete' on load;
// (can also send gen~ a max message 'initlut 1' to force reinitialisation externally)
if (initlut > 0) {
	i = 0;
	for (i = 0; i < fatso; i += 1) {				// lookup table
		v = scale(i, 0, (fatso - 1), -4, 4, 1);		// LUT is around -4..4
		v0 = tnhLam(v);								// tanh
		v1 = polysat(v);							// polynomial
		v2 = cnl(v);								// cubic
		v3 = parsat(v, 1, 1);						// parabolic
		poke(fatPete, v0, i, 0, index="samples", boundmode="clamp");
		poke(fatPete, v1, i, 1, index="samples", boundmode="clamp");
		poke(fatPete, v2, i, 2, index="samples", boundmode="clamp");
		poke(fatPete, v3, i, 3, index="samples", boundmode="clamp");
	}
	initlut = 0;									// deactivate once initialised
}

// INPUTS STAGE 1

InLeft = in1;													// master audio inputs
InRight = in2;

preMasterDelay = mstosamps(InitialDelay);						// 'pre' for feeding reverse playbacks and hold trap
// used to be downsampled [svf~] smoother, we prefered the sound & response, but...
MasterDelay = rsmooth(preMasterDelay, 0.1247);					// .1247 up for grabs; could be dynamic
//MasterDelay = lsmooth(preMasterDelay, 0.18705, 0.0001);		// .18705 up for grabs; could be dynamic
																// clip(preMasterDelay, vectorsize, samplerate)
MechanicalNoise = in3;											// Flutter, -0.72874..+0.72874 maximum movement

// -------	< GLOBAL BYPASS (FOR MODESELECTOR == 12, REVERB ONLY) / >
																//
// INITIALISATION STAGE											//
																//
Flutter = 0;													//
globalphase, writephase0, th1p = 0;								//
driveTape1L = 0;												//
TH1L, TH1R, TH2L, TH2R = 0;										//
VU1, VU2, VU3 = 0;												//
OutLeft, OutRight = 0;											//
FeedbackLeft, FeedbackRight = 0;								//
fbamp = 0;														//
sHMdelta = 0;													//
																//
if (M == 0) {													//
																//
// -------	< GLOBAL BYPASS (FOR MODESELECTOR == 12, REVERB ONLY) / >

	// INPUTS STAGE 2

	// holdsmooth & invhs must be expensive signal rate for smooth summing of stems
	holdsmooth = smpsmooth(H, 0.9995);				// 'Hold' button 0/1, sync, smoothed 0..1
	invhs = 1 - holdsmooth;							// simple inverse linear xfade

	// no mod for 'Hold'; Flutter is maximum ± 32.14 samples @ sr=44100Hz
	Flutter = H ? 0 : mstosamps(MechanicalNoise);

	inL = InLeft * invhs;							// dry L&R attenuated by 'Hold'
	inR = InRight * invhs;

	wdmixL = fbL + inL;								// feedback + dry
	wdmixR = fbR + inR;

	// FEEDBACK & HOLD STAGE

	// Magnetic update 1.1: maintain cheap(er) param rate here, even though it means a jumpy 'intensitysmooth' throughout
	prefeedamp = 0;
	if (q > 0) {
		if (tplgy != 3) {
			prefeedamp = 1.578;				// use more boost in quality == 1 || 2 to compensate envelope and oversampling
		} else {
			prefeedamp = 1.422;				// ...but if 'Dark' style...
		}	
	} else {	// q == 0
		if (tplgy != 3) {
			prefeedamp = 1.333;				// almost unity -> 1.3 = 130 % = original knob value
		} else {
			prefeedamp = 1.211;				// ...but if 'Dark' style...
		}
	}
	intensitysmooth = H ? 1 : intensity;
	postfeedamp = intensitysmooth * (H ? holdatten : prefeedamp);
	// only now use signal rate for fbL & fbR attenuation
	fbamp = cpsm(postfeedamp, 0.999);		// mul this fucker (or prefeedamp) if you have problems or if you disagree with me !
/*
	// Magnetic version 1:
	intensitysmooth = cpsm(mix(intensity, 1, holdsmooth), 0.999);	// stupid sig/param expensive - need better way
	if...
		...etc...
*/
	// Magnetic update 1.1: now all this is param rate...
	is2 = intensitysmooth * intensitysmooth;
	char2 = character * character;
/*	resmod0, resmod1 = 0;
	if (q == 2) {												// optional mod in 'quality == 2' mode
		is3 = is2 * is2;
		is4 = is3 * is3;
		resmod0 = (char2 * 0.08936) + (is4 * 0.944);			// modulate trap filter resonance
		resmod1 = character + 0.000001;
	} else {
		resmod0 = ((char2 * 0.08736) + is2) + 0.01;				// modulate trap filter resonance
		resmod1 = character + 0.01;
	}
*/
	resmod0 = ((char2 * 0.08736) + is2) + 0.01;					// modulate trap filter resonance
	resmod1 = character + 0.01;

	// AMP SIMULATION STAGE

	// -------	< AMP MOD SUB BYPASS / >

	satOutL, satOutR = 0;								// init pre-hold
	env2 = 0;
	if (holdsmooth < 0.999984) {						// if 'hold' != ..1.. compute	// 0.999999 == -120dB / 0.999984 == -96dB

		// ENVELOPE STAGE

		/*
		sometimes we overshoot unity gain into the saturator. this is analogue tape / tube
		like saturation behaviour. with transient-heavy material and 'character' all the way
		up to 1 these overshoots are 'fast' and can spike at up to +4dB, whereas with character
		low, overshoots reach approx maximum +1dB, although with sustained / less transient
		input material overshooting is rarely engaged (at lower 'character' settings). post
		saturation gain compensation is on an envelope follower, either the same as agc or
		one with the same attack but a slower decay (relative to 'character'), selected by
		the 'compensate' param. post gain is never compensated for entirely, sometimes letting
		some hot signal through the feedback loop.
		*/

		modifier, agc, compen = 0;
		Amix, Cmix, Mmix, S = 0;
		if (q > 0) {			// q == 1 || 2, use envelope

			follow = (inL + inR) * sqrt1_2;					// L + R average for envelope following

			env1, skew = 0;									// inits
			if (intensity < 1) {							// only compute for variable intensity

				// dynamic depending on 'character'
				// char2 = character * character; // 0 = smooth .. 1 = more (jumpy) transient overshoots into saturator
				range = (char2 * 0.876) + 0.25;
				attack = ((1 - character) * 0.3863) + 0.0157;
				decay = ((1 - char2) * 0.5514) + 0.0236;
				decaymult = (character * 0.056) + 0.104;

				env1, env2 = p_Env(follow, range, attack, decay, decaymult, ew);	// env1 0..1

				agc = (env1 * 0.719233) + 0.803526;			// approx -2 .. +3.6 dB range
				// i like 'ew' switching but might need to ditch it...
				if (ew > 0) {
					precompen = (cw > 0) ? ((env2 * 0.719233) + 0.794328) : agc;
					compen = minimum(1 / minimum(precompen, (dbtoa((char2 * 2) + 1))), 1.122018);
				} else {
					compen = minimum(1 / minimum(agc, (dbtoa(((1 - char2) * 2) + 1.4))), 1);
				}
				modcompress = (character * 0.28) + 0.51;
				// global modifier for non-linearities
				modifier = ((env1 * modcompress) + (1 - modcompress)) + (character * 0.347);	// can be > 1, but clipped elsewhere
				skew = (char2 * 0.019) + 0.001;

				// PURIFY STAGE

				/*
				but... ...at highest intensity-feedback ranges, settings gradually
				xfade to pure non-linearity with no modifier or envelope following
				*/

				if (intensity > 0.384615) {	// 50 %					// use original
					// Magnetic update 1.1: now param rate...
					intense = (maximum(intensitysmooth, 0.384615) - 0.384615) * 1.624999;	// * (1 / (1 - 0.384615))

					Amix	= mix(agc, 		1.001152,	intense);	// 0.988553
					Cmix	= mix(compen, 	0.988553,	intense);	// 1.001152
					Mmix	= mix(modifier, 0.491438,	intense);
					S		= 0;
				} else {
					Amix	= agc + 0;
					Cmix	= compen + 0;
					Mmix	= modifier + 0;
					S		= skew + 0;
				}
			// if user turns 'Intensity' down after reaching '1' (130%) the envelope follower history has nothing in it
			// so there is a delay to normal operation; however we accept this here for purposes of cpu saving;
			// only really noticable if large jumps are automated anyway, and 'feels' like 'analogue inertia' ;-)
			} else {	// intensity == 1 (130%)			// only compute for variable intensity
				Amix = 1.001152;									// 1.001152
				Cmix = 0.988553;									// 0.988553
				Mmix = 0.491438;
				agc = Amix;											// these declarations may look stupid but...
				compen = Cmix;										// ...there is a Gen compiler bug when declaring on same line
				modifier = Mmix;									// (thanks Torstein!)
				skew, env1, env2 = 0;
			}

		} else {	// q == 0								// use no envelope, volatile state with easy feedback
			modifier = sqrt1_2;								// raise for 1x (because always static for 1x)
			Amix = 1.001152;
			Cmix = 0.988553;
			env2 = 0;
			if (nl == 2) {									// special hack for (q==0 && nl==2)
				agc = Amix;
				compen = Cmix;
			}
		}

		// SATURATION STAGE

		doppelAgc = dbtoa(modifier * (character * 3));		// not 'Mmix' (& character as mini 3dB+ 'drive')

		satInL = (wdmixL * Amix) * doppelAgc;
		satInR = (wdmixR * Amix) * doppelAgc;

		satL, satR = 0;										// init
		if (q == 2) {										// realtime compute non-linearities (+ realtime modifiers)
			satL = sat(nl, satInL,  Mmix);					// tanh+tape approx / polynom / cubic / parabolic; ('nl')
			satR = sat(nl, satInR, (Mmix - S));				// * 0.954993;
		} else {	// q == 0 || 1							// only use local lookup tables, cheap(er) cpu; default
			dvlL	= satInL * 0.25;								// LUTs are around -4..4
			dvlR	= satInR * 0.25;
			drivelutL, drivelutR, cp = 0;					// internal init
			// non-linearity usage slightly different
			if (nl == 2) {									// 'cubic' non-lin (for ‘LoFi’)
				cp = compen;								// & compensate
				drivelutL	= clip(dvlL,	-1, 1) * agc;			// + lo-fi fuck
				drivelutR	= clip(dvlR,	-1, 1) * agc;			// clip because agc can be lower than '1'
			} else {										// tanh approx (default) / polynom / parabolic
				cp			= 1;							// factor out
				drivelutL	= dvlL;									// pass
				drivelutR	= dvlR;									// should not need to clip, but do we ?
			}
			// switch interpolations for q 0 / 1
			if (q > 0) {	// q == 1
				// all cubic interp, shaped for alternate high frequency response
				satL	= lookup(fatPete, drivelutL,	nl, interp="cubic",		boundmode="clamp") * cp;	// {boundmode} not working ?
				satR	= lookup(fatPete, drivelutR,	nl, interp="cubic",		boundmode="clamp") * cp;
			} else { 		// q == 0
				// all cosine not cubic, shaped for high frequency response; default
				satL	= lookup(fatPete, drivelutL,	nl, interp="cosine",	boundmode="clamp") * cp;
				satR	= lookup(fatPete, drivelutR,	nl, interp="cosine",	boundmode="clamp") * cp;
			}
		}

		if (q != 0) {	// compensate 'quality' settings
			doppelCompen	= 0.944061 / doppelAgc;			// compensation trick
			satOutL = (satL * Cmix) * doppelCompen;
			satOutR = (satR * Cmix) * doppelCompen;
		} else {		// q == 0
			doppelCompen	= minimum((1.412538 / doppelAgc), 1);	// because 'modifier' is static for 1x (now character 'drive' works)
			satOutL = (satL * Cmix) * doppelCompen;
			satOutR = (satR * Cmix) * doppelCompen;
		}

	} else {												// if 'Hold' == 1, do not compute
		satOutL, satOutR = 0;
		env2 = 0;
	}

	// ------- < / AMP MOD SUB BYPASS >

	// MIX STAGE

	// dry L&R are attenuated out earlier in the chain using 'hold' (lines 508..522)
	TapeL = (wdmixL * holdsmooth) + (satOutL * invhs);
	TapeR = (wdmixR * holdsmooth) + (satOutR * invhs);

	// RECORD TO TAPE STAGE

	// phases
	globalphase = wrap((globalaccum + 1), 0, tdim);
	writephase0 = wrap(globalphase, 0, halftdim);
	writephase1 = writephase0 + halftdim;
	// because we rewrap for modifiers later...
	readdelay = wrap((globalphase - MasterDelay), 0, tdim);
	// for Hold sync trapezoid, but with variable delay allowed (or should re-sync 'readdelay' here?)
	phasetrap = clip(((wrap(readdelay, 0, MasterDelay)) / MasterDelay), 0, 1);	// or 'preMasterDelay' ?

	// circular write
	poke(Tape, TapeL, writephase0, 0, index="samples");		// [splat] broken in GenExpr ?
	poke(Tape, TapeR, writephase0, 1, index="samples");
	poke(Tape, TapeL, writephase1, 0, index="samples");
	poke(Tape, TapeR, writephase1, 1, index="samples");

	// CAPSTAN & PINCH ROLLER STAGE

	MasterPhase = 0;															// init
	if (H > 0) {																// no mod for 'hold'
		MasterPhase = wrap(readdelay, 0, tdim);												// master read phase (no need for wrap ?)
	} else {
		if (wow > 0.000002) {													// only compute if called ('MegaWow')
			wowMax = mstosamps(4.249);											// your guess goes here... the measurements suck
			// (the wow should at least seem more pronounced during
			// smoother sustained material with less regular transients):
			wctrl = smpsmooth(((1 - env2) * wow), 0.9995);						// how much % ? (env2 = 0..1) + smoother
			// ^^ (needs to be interjected as Flutter & Wow are on the same slider)
			feedlfo = clip((head1 / tdim), 0, 1);								// head phase, tape size; regular but affected by Flutter...
			wlfo = sinApp01(feedlfo) * ((triangle(phasetrap) * 0.45) + 0.225);	// ...& loop size; regular but interupted
			ctrllfo = wlfo * wctrl;												// (would prefer cubic rand osc for wlfo)
			capstanwow = (dir > 0) ? (ctrllfo * 0.225) : ctrllfo;				// direction, less wow in reverse...
			// very slight bias with... character param
			wowActual = capstanwow + ((character * 0.0003) - 0.000075);			// == 0 bias @ character default (0.25)
			MasterPhase = wrap((readdelay + (wowActual * wowMax)), 0, tdim);				// master read phase + wow modulation
			// ^^ fixed rate, wobbles depending on the capstan wow, maximum -wowMax .. +wowMax, depth 187.4 samples @ sr=44100, but modded
		} else {
			MasterPhase = wrap(readdelay, 0, tdim);											// master read phase (no need for wrap ?)
		}
	}

	// DELAYS AND MODESELECTOR STAGE

	trapmul = 0;													// inits
	delay2, delay3, delay4 = 0;
	driveRev1L, driveRev1R = 0;
	munge1, munge2 = 0;
	AllPlayL, AllPlayR = 0;
	if (H > 0) {
		munge1, munge2 = 0;											// no mod for 'hold'
	} else {
		munge1 = mungephase.read(606, 0);							// (incredibly serious amount of samples)
		munge2 = mungephase.read(808, 1);
	}
	if (dir == 1) {													// if reverse (switching in Magnetic is not quantised)
		trapphase, tramp = 0;
		postMasterDelay = sah(preMasterDelay, syncRevMaster, 0.5);	// sync for reverse, not smooth
		delayphase, delaydelta = counter(1, 0, postMasterDelay);	// local phase for reverses, but synced (thanks alex)
		// switches for reverse (loop beat-synced if 'Repeat' dial in 'notevalues' mode, not necessarily if in free 'ms' mode)

		if (revstyle == 0) {										// if reverse play heads only
			reverseheads = tdim - MasterPhase;
			// setup rewrap
			halftdimm2 = halftdim - 2;
			tramp = 0.077;//0.015;
			pretraph = wrap(((writephase0 + 1) - reverseheads), 0, halftdimm2) / halftdimm2;
			trapphase = wrap((pretraph - tramp), 0, 1);				// offset for correct trapezoiding around jump point
			// actual delay phase	// rewrap inside write heads (for avoiding write heads)
			rphase1 = wrap(reverseheads, (writephase0 + 1), (writephase1 - 1));
			// to Rev Tape 1 L
			driveRev1L = (wrap((rphase1 + Flutter), 0, tdim));		// delay 1...
			// to Rev Tape 1 R
			driveRev1R = (wrap((rphase1 + munge1), 0, tdim));
			// to Tape 2 L
			delay2 = (postMasterDelay + munge2);
			// to Tape 2 R (and 'holder')
			delay3 = (postMasterDelay + Flutter);
			// delay4 = delay3 + 1;
			delay4 = delay3 + 1;									// only needed for head 3

		} else {	// revstyle == 1 || 2							// if reverse delay phase and step @ each ramp (1 == default)
			reverseloop = sah(postMasterDelay, delaydelta, 0.5, init=1) - delayphase;
			trapphase = clip((delayphase / postMasterDelay), 0, 1);	// paranoid clip ?
			tramp = 0.04;
			// actual delay phase, different for 1 || 2
//			rphase1 = 0;											// init switch

/*			if (revstyle == 2) {									// reverse from playhead

				wp0p1 = writephase0 + 1;
				rphase0 = (reverseloop - 1) + ((sah(MasterPhase, delaydelta, 0.5, init=1) - postMasterDelay) - 1);	// PLUS !
				rphase1 = wrap(rphase0, ((rphase0 > wp0p1) ? wp0p1 : (writephase0 - halftdim)), MasterPhase);

			} else {	// revstyle == 1							// reverse from writehead (original & default)
*/
				rphase1 = (reverseloop - 1) + (sah(MasterPhase, delaydelta, 0.5, init=1) - 1);	// PLUS !
//			}

			// to Rev Tape 1 L
			driveRev1L = (wrap((rphase1 + Flutter), 0, tdim));		// delay 1...
			// to Rev Tape 1 R
			driveRev1R = (wrap((rphase1 + munge1), 0, tdim));
			// to Tape 2 L
			delay2 = (postMasterDelay + munge2);
			// to Tape 2 R (and 'holder')
			delay3 = (postMasterDelay + Flutter);
			// delay4 = delay3 - 1;
			delay4 = delay3 - 1;									// only needed for head 3

		}

		trapmul = hTrap(trapphase, 0, 1, tramp, (1 - tramp));		// trapezoid (up/down should really be properly dynamic)
		syncRevMaster = delta(trapphase) < 0;						// for MasterDelay sync changes when in reverse modes

	} else {														// direction == 0 (forwards), default
		// to Tape 2 L
		delay2 = (MasterDelay + munge2);
		// to Tape 2 R (and 'holder')
		delay3 = (MasterDelay + Flutter);
		// delay4 = delay3 - 1;
		delay4 = delay3;											// only needed for head 3
		// just in case
		trapmul = 1;
		syncRevMaster = 1;
	}

	// actual delay phase
	faze = MasterPhase;												// delay 1...
	// to real Tape 1 L
	driveTape1L = (wrap((faze + Flutter), 0, tdim));
	// to real Tape 1 R
	driveTape1R = (wrap((faze + munge1), 0, tdim));
	// Hold phase - Magnetic update 1.1; I hate this 'multrap' crap, it should work without it, but such is life
	multrap = 1;
	ss = 0;
	if (H > 0) {
		pu, arriere = 0;
		if (preMasterDelay < 10000) {								// modify points for small delay times
			pMDclip = clip(preMasterDelay, 0, 10000);				// (should really be based on ms not samples but...)
			rier = pMDclip / 10000;
			pu = scale(pMDclip, 0, 10000, 0, 0.05, 2.438);			// [scale] is lazy, + debatable exponent
			arriere = clip((1 - (rier * rier)), 0, 0.501);
			ss = 1;													// safe mode on
		} else {													// static for larg(er) delay times
			pu = 0.05;
			arriere = 0;
			ss = 0;													// safe mode off
		}
		nwod = 1 - pu;
		multrap = hTrap(phasetrap, arriere, 1, pu, nwod);			// trapezoid (up/down and arriere sort of dynamic hack)
	} else {
		multrap = 1;												// no trapezoid (only for 'Hold')
	}
	sHMdelta = delta(phasetrap) < 0;								// update syncHoldMaster
	// 'real' Tape Head 1 (always on & always forward, multrap for 'Hold'-on only) - boundmode fixes phases for me but expensive
	AllPlayL	= sample(Tape, driveTape1L, 0, index="samples", interp="cubic", boundmode="wrap") * multrap;
	AllPlayR	= sample(Tape, driveTape1R, 1, index="samples", interp="cubic", boundmode="wrap") * multrap;
	// dare for reverse, truth for forwards
	if (dir > 0) {
		// trapezoid saving cpu on double buffer method but not as good; & boundmode fixes phases for me but expensive
		TH1L	= sample(Tape, driveRev1L, 0, index="samples", interp="cosine", boundmode="wrap") * trapmul;
		TH1R	= sample(Tape, driveRev1R, 1, index="samples", interp="cosine", boundmode="wrap") * trapmul;
	} else {
		TH1L	= AllPlayL;
		TH1R	= AllPlayR;
	}
	// play safe in 'Hold'-mode-with-small-delay-times (shame as it adds cpu...)
	if (H > 0) {
		if (ss > 0) {
			hLw = scale(MasterDelay, 10, 250, 2570, 10700, 2);		// really dumb
			TH1L = ePoleZeroLPHP(0, TH1L, hLw);
			TH1R = ePoleZeroLPHP(0, TH1R, hLw);
			TH1L = softStatic(TH1L);
			TH1R = softStatic(TH1R);
		} else {
			TH1L = softStatic(TH1L);								// probably dumb
			TH1R = softStatic(TH1R);
		}
	}

	// matt's Mode Selectors
	DL, DR = 0;													// inits
	if (Mode == 1) {											// ONLY COMPUTE IF CALLED
		DL = TH1L;
		DR = TH1R;
		TH2L = TH1L;											// always write 'something' to unused delay lines...
		TH2R = TH1R;
		TH3L = 0;
		TH3R = 0;
		VU1 = TH1L + TH1R;										// metering (all addition compensated later in chain)
		VU2, VU3 = 0;
	} else if (Mode == 2) {
		if (H > 0) {
			TH2L = head2L.read(delay2, interp="cubic");
			TH2R = head2R.read(delay3, interp="cubic");			// no mod for 'hold'
		} else {
			TH2L = head2L.read(delay2, interp="cubic");
			TH2R = head2R.read((delay3 - 404), interp="cubic");	// for matt
		}
		DL = TH2R;												// N.B. swaps...
		DR = TH2L;
		TH3L = 0;
		TH3R = 0;
		VU2 = TH2L + TH2R;
		VU1, VU3 = 0;
	} else if (Mode == 3) {										// 3 is not like 7 because such is life
		TH2L = head2L.read(delay2, interp="cubic");
		TH2R = head2R.read(delay3, interp="cubic");
		TH3L = head3L.read(delay4, interp="cubic");
		TH3R = head3R.read(delay4, interp="cubic");
		DL = TH3R;												// N.B. swaps...
		DR = TH3L;
		VU3 = TH3L + TH3R;
		VU1, VU2 = 0;
	} else if (Mode == 4) {
		TH2L = head2L.read(delay2, interp="cubic");
		TH2R = head2R.read(delay3, interp="cubic");
		TH3L = head3L.read(delay4, interp="cubic");
		TH3R = head3R.read(delay4, interp="cubic");
		DL = plus2A(TH2R, TH3R) * hscale(1, Hold);
		DR = plus2A(TH2L, TH3L) * hscale(1, Hold);
		VU2 = TH2L + TH2R;
		VU3 = TH3L + TH3R;
		VU1 = 0;
	} else if (Mode == 5) {	// default on load					// with Reverb from here...
		DL = TH1L;
		DR = TH1R;
		TH2L = TH1L;
		TH2R = TH1R;
		TH3L = 0;
		TH3R = 0;
		VU1 = TH1L + TH1R;
		VU2, VU3 = 0;
	} else if (Mode == 6) {
		if (H > 0) {
			TH2L = head2L.read(delay2, interp="cubic");			// no mod for 'hold'
			TH2R = head2R.read(delay3, interp="cubic");
		} else {
			TH2L = head2L.read((delay2 + 404), interp="cubic");	// for fun
			TH2R = head2R.read(delay3, interp="cubic");
		}
		DL = TH2R;
		DR = TH2L;
		TH3L = 0;
		TH3R = 0;
		VU2 = TH2L + TH2R;
		VU1, VU3 = 0;
	} else if (Mode == 7) {										// 7 is not like 3 because such is life
		if (H > 0) {
			TH2L = head2L.read(delay2, interp="cubic");
			TH2R = head2R.read(delay3, interp="cubic");			// no mod for 'hold'
		} else {
			TH2L = head2L.read(delay2, interp="cubic");
			TH2R = head2R.read((delay3 - 404), interp="cubic");	// for pete
		}
		TH3L = head3L.read(delay4, interp="cubic");
		TH3R = head3R.read(delay4, interp="cubic");
		DL = TH3R;
		DR = TH3L;
		VU3 = TH3L + TH3R;
		VU1, VU2 = 0;
	} else if (Mode == 8) {
		TH2L = head2L.read(delay2, interp="cubic");
		TH2R = head2R.read(delay3, interp="cubic");
		DL = plus2A(TH1L, TH2R) * hscale(1, Hold);
		DR = plus2C(TH1R, TH2L) * hscale(3, Hold);
		TH3L = 0;
		TH3R = 0;
		VU1 = TH1L + TH1R;
		VU2 = TH2L + TH2R;
		VU3 = 0;
	} else if (Mode == 9) {
		TH2L = head2L.read(delay2, interp="cubic");
		TH2R = head2R.read(delay3, interp="cubic");
		TH3L = head3L.read(delay4, interp="cubic");
		TH3R = head3R.read(delay4, interp="cubic");
		DL = plus2A(TH2R, TH3R) * hscale(1, Hold);
		DR = plus2C(TH2L, TH3L) * hscale(3, Hold);
		VU2 = TH2L + TH2R;
		VU3 = TH3L + TH3R;
		VU1 = 0;
	} else if (Mode == 10) {
		TH2L = head2L.read(delay2, interp="cubic");
		TH2R = head2R.read(delay3, interp="cubic");
		TH3L = head3L.read(delay4, interp="cubic");
		TH3R = head3R.read(delay4, interp="cubic");
		DL = plus2A(TH1L, TH3R) * hscale(1, Hold);
		DR = plus2C(TH1R, TH3L) * hscale(3, Hold);
		VU1 = TH1L + TH1R;
		VU3 = TH3L + TH3R;
		VU2 = 0;
	} else if (Mode == 11) {
		TH2L = head2L.read(delay2, interp="cubic");
		TH2R = head2R.read(delay3, interp="cubic");
		TH3L = head3L.read(delay4, interp="cubic");
		TH3R = head3R.read(delay4, interp="cubic");
		DL = plus2B(((TH1L + TH2R) * 0.666667), TH3R) * hscale(2, Hold);	// * 0.5 / sqrt1_2 (0.666667) ?!
		DR = (plus2D((plus2C(TH1R, TH2L) * hscale(3, Hold)), TH3L)) * hscale(3, Hold);
		VU1 = TH1L + TH1R;
		VU2 = TH2L + TH2R;
		VU3 = TH3L + TH3R;
	} else {	// Mode == 12									// solo Reverb
		DL, DR = 0;
		TH2L, TH2R = 0;											// TH2L & TH2R can be zero, parent muted anyway
		TH3L, TH3R = 0;
		VU1, VU2, VU3 = 0;
	}

	// RECORD HEAD TRAP FILTERS AND HYSTERISIS STAGE

	// ------- < TRAP FILTERS SUB BYPASS / >

	if (H > 0) {												// BYPASS IN 'HOLD' MODE

		OutLeft = DL;											// PASS
		OutRight = DR;
		if (dir > 0) {											// swap feedback source for reverse
			FeedbackLeft = AllPlayL;
			FeedbackRight = AllPlayR;		
		} else {
			FeedbackLeft = OutLeft;
			FeedbackRight = OutRight;
		}

	} else {													// FILTER

		// freqmult & freqatten smoothing too expensive ?
		hat1, hat2 = 0;											// init hysterisis attacks
		freqatt = 0;											// for use with tplgy == 0 & dir == 1
		yL, yR = 0;
		if (tplgy != 0) {	// tplgy == 1 || 2 || 3
			rmul = ((intensitysmooth * resmod1) * 0.99) + 0.01;	// for 'scream' mode Hz mod
			rm0 = ((maximum(resmod0, 0) * 0.45264) + 0.04);		// post pow scale
			// rmul & rm0 smoothing too expensive ?
			rmul = smpsmooth(rmul, 0.9995);
			rm0 = smpsmooth(rm0, 0.9995);
			// technically 'rm105' (or resmod1) should also be smoothed
			rm105 = resmod1 * 0.666667;							// extra resonance for matt
			rm1025 = rm105 * 0.666667;
			rm01 = rm0 + rm105;
			// 'rmul' & 'freqmult' etc tuned by ear
			freqatt = 0;
			if (tplgy == 1) {										// good onepole, dirty HP				// 'LoFi'
				freqmult = (intensitysmooth * -0.06) + 0.72;
				freqmult = smpsmooth(freqmult, 0.9995);
				yL0 = ePoleZeroLPHP(0, DL, (2240 * freqmult));
				yR0 = ePoleZeroLPHP(0, DR, (2240 * freqmult));
				yL = LoresHipass(yL0, (35 * rmul), (rm0 * rmul));
				yR = LoresHipass(yR0, (36 * rmul), (rm0 * rmul));
				hat1 = 0.0001;
				hat2 = 0.0001;
			} else if (tplgy == 2) {								// sallen & key then onepole			// 'Old'
				freqmult = (((clip(is2, 0.5, 1) * 2) - 1) * -0.07) + 1;
				freqmult = smpsmooth(freqmult, 0.9995);
				rmod = rm0 * ((is2 * -0.875) + 1);
				rmd = minimum(((rm01 * ((is2 * -0.875) + 1)) + rm1025), 0.97);
				yL0 = SallenAndKey(0, DL, (3699 * freqmult), rmod);
				yR0 = SallenAndKey(0, DR, (3699 * freqmult), rmod);
				yL = eAllPoleLPHP6(1, (yL0 * 0.922571), (214 * rmd));	// 142	// attenuate for s&k
				yR = eAllPoleLPHP6(1, (yR0 * 0.922571), (214 * rmd));	// 142
				hat1 = 0.002;
				hat2 = 0.002;
			} else {	// tplgy == 3								// sallen & key then sallen & key		// 'Dark'
				freqmult = intensitysmooth + 1;
				freqmult = smpsmooth(freqmult, 0.9995);
				rmod = rm0 * (1 - (is2 * 0.5));
				rmd = ((minimum((rm01 * (1 - (is2 * 0.5))), 0.97) * rmul) * is2) * 0.808;
				yL0 = SallenAndKey(0, DL, (3699 * freqmult), rmod);
				yR0 = SallenAndKey(0, DR, (3699 * freqmult), rmod);
				sL = SallenAndKey(1, (yL0 * 0.822243), (145 * rmul), maximum(rmd, 0.11));	// 74
				sR = SallenAndKey(1, (yR0 * 0.822243), (148 * rmul), maximum(rmd, 0.11));	// 73
				yL = simpSat(sL * 0.822243) * 0.906776;				// on purpose so that 'Dark' feedback is always heavy
				yR = simpSat(sR * 0.822243) * 0.906776;
				hat1 = 0.003;
				hat2 = 0.002;
			}
		} else {	// tplgy == 0									// onepoles chain (original default)	// '201'
			freqatt = smpsmooth(freqatten, 0.9995);					// smooth only if called
			yL0 = eAllPoleLPHP6(0, DL, (2000 * freqatt));			// freqatten for topology '0' only
			yR0 = eAllPoleLPHP6(0, DR, (2000 * freqatt));
			yL = eAllPoleLPHP6(1, yL0, 147);
			yR = eAllPoleLPHP6(1, yR0, 147);
			hat1 = 0.001;
			hat2 = 0.0002;
		}

		preLeft, preRight = 0;
		if (dir == 1) {							// lose hysterisis in 'reverse' modes
			freqa = 0;
			if (tplgy == 0) {					// if already being smoothed, use that...
				freqa = freqatt;
			} else { // tplgy != 0				// ...if not, smooth it
				freqa = smpsmooth(freqatten, 0.9995);
			}
			preLeft = yL;
			preRight = yR;
			OutLeft = dcblock(softStatic(preLeft));
			OutRight = dcblock(softStatic(preRight));
			// if reverse, cheat with dry playheads to feedbacks (and 'tplgy 0' system of course)
			prefbL = eAllPoleLPHP6(0, AllPlayL, (2000 * freqa));
			prefbR = eAllPoleLPHP6(0, AllPlayR, (2000 * freqa));
			FeedbackLeft = eAllPoleLPHP6(1, prefbL, 147);
			FeedbackRight = eAllPoleLPHP6(1, prefbR, 147);
		} else {								// direction == 0, default with hysterisis
			// hysterisis attacks skewed on purpose
			preLeft = yL * feederCompression(inL, hat1, hysterisis);	// output expansion multipliers
			preRight = yR * feederCompression(inR, hat2, hysterisis);
			OutLeft = dcblock(softStatic(preLeft));
			OutRight = dcblock(softStatic(preRight));
			FeedbackLeft = OutLeft;				// if forward, proper full loop system feedbacks
			FeedbackRight = OutRight;
		}
	}

	// ------- < / TRAP FILTERS SUB BYPASS >

// -------	< / GLOBAL BYPASS (FOR MODESELECTOR == 12, REVERB (convolution) ONLY) >
																//
} else {														//
																//
	Flutter = 0;												//
	globalphase, writephase0, th1p = 0;							//
	driveTape1L = 0;											//
	TH1L, TH1R, TH2L, TH2R = 0;									//
	VU1, VU2, VU3 = 0;											//
	// if 'mode'=='12' && 'reverb routing'=='post', mirror inputs to outputs
	if (rvRo == 1) {	// == 'post', pass inputs				//
		OutLeft, OutRight = InLeft, InRight;					//
	} else {			// != 'post', ignore					//
		OutLeft, OutRight = 0;									//
	}															//
	FeedbackLeft, FeedbackRight = 0;							//
	fbamp = 0;													//
	sHMdelta = 0;												//
																//
}																//
																//
// -------	< / GLOBAL BYPASS (FOR MODESELECTOR == 12, REVERB (convolution) ONLY) >

// OUTPUTS

out1, out2 = OutLeft, OutRight;								// masters L & R; master levels in MSP [matrix~]

/*
if (q == 2) {
	out3 = wowActual * hysterisisActual;					// for modulation
} else {
	out3 = 0;
}
*/
//out4, out5, out6 = VU1, VU2, VU3;
out3, out4, out5 = VU1, VU2, VU3;							// for metering, to be attenuated and downsampled externally

// UPDATES

globalaccum = globalphase;		// master phase
tapephase = writephase0;		// in wrap phase
head1 = driveTape1L;			// for wow lfo
syncHoldMaster = sHMdelta;		// sync 'Hold' changes

mungephase.write(Flutter);		// skew flutter heads

head2L.write(TH1L);
head2R.write(TH1R);
head3L.write(TH2R);											// N.B. swap
head3R.write(TH2L);

fbL = fixdenorm(FeedbackLeft * fbamp);						// scientific
fbR = fixdenorm(FeedbackRight * fbamp);

/*
we get self-oscillation with small 'delay' times, 'noise' amounts > 0. and 'intensity' increasing above about 0.9;
the speed and power of oscillation and tone quality depends on 'Mode Selector', 'trap filters' and 'character';
with correct noise and other settings, delay times need not be small to achieve the oscillation - can take time to build.
reacts slightly different in each 'quality' mode; quality == 0 (default 1x), system is volatile and feeds back easily.
*/
