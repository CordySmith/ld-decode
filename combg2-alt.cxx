/* LD decoder prototype, Copyright (C) 2013 Chad Page.  License: LGPL2 */

#include "ld-decoder.h"
#include "deemp.h"

// capture frequency and fundamental NTSC color frequency
//const double CHZ = (1000000.0*(315.0/88.0)*8.0);
//const double FSC = (1000000.0*(315.0/88.0));

bool pulldown_mode = false;
int ofd = 1;
bool image_mode = false;
char *image_base = "FRAME";
bool bw_mode = false;

bool cwide_mode = false;

bool f_oneframe = false;

// NTSC properties
const double freq = 4.0;
const double hlen = 227.5 * freq; 
const int hleni = (int)hlen; 

const double dotclk = (1000000.0*(315.0/88.0)*freq); 

const double dots_usec = dotclk / 1000000.0; 

// values for horizontal timings 
const double line_blanklen = 10.9 * dots_usec;

double irescale = 327.67;
double irebase = 1;
inline uint16_t ire_to_u16(double ire);

// tunables

int linesout = 480;

double brightness = 240;

double black_ire = 7.5;
int black_u16 = ire_to_u16(black_ire);
int white_u16 = ire_to_u16(100); 
bool whiteflag_detect = true;

double nr_y = 4.0;
double nr_c = 0.0;

inline double IRE(double in) 
{
	return (in * 140.0) - 40.0;
}

// XXX:  This is actually YUV.
struct YIQ {
        double y, i, q, m, a;

        YIQ(double _y = 0.0, double _i = 0.0, double _q = 0.0) {
                y = _y; i = _i; q = _q; 
		m = a = 0.0;
        };

	YIQ operator*=(double x) {
		YIQ o;

		o.y = this->y * x;
		o.i = this->i * x;
		o.q = this->q * x;

		return o;
	}
	
	YIQ operator+=(YIQ p) {
		YIQ o;

		o.y = this->y + p.y;
		o.i = this->i + p.i;
		o.q = this->q + p.q;

		return o;
	}
	
	void ToPolar() {
		m = ctor(i, q); 
		a = atan2(q, i); 
	}

	void FromPolar() {
		i = m * cos(a);
		q = m * sin(a);
	}
};

double clamp(double v, double low, double high)
{
        if (v < low) return low;
        else if (v > high) return high;
        else return v;
}

inline double u16_to_ire(uint16_t level)
{
	if (level == 0) return -100;
	
	return -60 + ((double)(level - irebase) / irescale); 
}

struct RGB {
        double r, g, b;

        void conv(YIQ _y) {
               YIQ t;

		double y = u16_to_ire(_y.y);
		y = (y - black_ire) * (100 / (100 - black_ire)); 

		double i = (_y.i) / irescale;
		double q = (_y.q) / irescale;

                r = y + (1.13983 * q);
                g = y - (0.58060 * q) - (i * 0.39465);
                b = y + (i * 2.032);
#if 0
		double m = brightness / 100;

                r = clamp(r * m, 0, 255);
                g = clamp(g * m, 0, 255);
                b = clamp(b * m, 0, 255);
#else
		double m = brightness * 256 / 100;

                r = clamp(r * m, 0, 65535);
                g = clamp(g * m, 0, 65535);
                b = clamp(b * m, 0, 65535);
#endif
                //cerr << 'y' << y.y << " i" << y.i << " q" << y.q << ' ';
     };
};

inline uint16_t ire_to_u16(double ire)
{
	if (ire <= -60) return 0;
	
	return clamp(((ire + 60) * irescale) + irebase, 1, 65535);
} 

typedef struct cline {
	YIQ p[910];
} cline_t;

int write_locs = -1;

class Comb
{
	protected:
		int linecount;  // total # of lines process - needed to maintain phase
		int curline;    // current line # in frame 

		int framecode;
		int framecount;	

		bool f_oddframe;	// true if frame starts with odd line

		long long scount;	// total # of samples consumed

		int fieldcount;
		int frames_out;	// total # of written frames
	
		uint16_t frame[1820 * 530];

		uint16_t output[744 * 505 * 3];
		uint16_t obuf[744 * 505 * 3];

		uint16_t rawbuffer[3][844 * 505];
		double LPraw[3][844 * 505];
		double K3D[844 * 505];

		cline_t wbuf[3][525];
		cline_t cbuf[525];
		cline_t prevbuf[525];
		Filter *f_i, *f_q;
		Filter *f_wi, *f_wq;

		Filter *f_hpy, *f_hpi, *f_hpq;
		Filter *f_hpvy, *f_hpvi, *f_hpvq;

		void LPFrame(int fnum)
		{
			for (int l = 24; l < 505; l++) {
				for (int h = 32; h < 844; h++) {
					LPraw[fnum][(l * 844) + h - 16] = f_lpf10h.feed(rawbuffer[fnum][(l * 844) + h]);
				}
			}
		}
		
		void Split(int f) 
		{
			double lp[844 * 505];
			
			for (int l = 0; l < 24; l++) {
				uint16_t *line = &rawbuffer[f][l * 844];	
					
				for (int h = 4; h < 840; h++) {
					wbuf[f][l].p[h].y = line[h]; 
					wbuf[f][l].p[h].i = 0; 
					wbuf[f][l].p[h].q = 0; 
				}
			}

			for (int l = 24; l < 505; l++) {
				uint16_t *line = &rawbuffer[f][l * 844];	
				bool invertphase = (line[0] == 16384);
				
				for (int h = 8; h < 844; h++) {
					double si, sq;
					int phase = h % 4;
					double y = line[h];
					double c = (((line[h + 2] + line[h - 2]) / 2) - line[h]) / 2; 
					if (!invertphase) c = -c;

					switch (phase) {
						case 0: si = c; break;
						case 1: sq = -c; break;
						case 2: si = -c; break;
						case 3: sq = c; break;
						default: break;
					}

					wbuf[f][l].p[h].y = y;
					if (cwide_mode) {
						wbuf[f][l].p[h - 5].i = bw_mode ? 0 : f_wi->feed(si); 
						wbuf[f][l].p[h - 5].q = bw_mode ? 0 : f_wq->feed(sq); 
					} else {
						wbuf[f][l].p[h - 8].i = bw_mode ? 0 : f_i->feed(si); 
						wbuf[f][l].p[h - 8].q = bw_mode ? 0 : f_q->feed(sq); 
					}	
				}	

				double m[844], a[844];
				for (int h = 8; h < 836; h++) {
					wbuf[f][l].p[h].ToPolar();
					if (l == 100) {
				//		cerr << h << ' ' << cbuf[l].p[h].y << ' ' << cbuf[l].p[h].i << ' ' << cbuf[l].p[h].q << ' ' << cbuf[l].p[h].m << ' ' << cbuf[l].p[h].a << endl; 
					}
				}	
			}
		}	
		
		void DoComb(int dim, int f) {
			for (int l = 25; l < 504; l++) {

				double k;
			
				YIQ y[844];

				// 2D computation
				for (int h = 8; h < 836; h++) {
					double adiff1 = absWrapAngle(wbuf[f][l + 2].p[h].a - wbuf[f][l - 2].p[h].a);
					double k1 = 1.0 - (adiff1 / M_PIl);
					double cang = WrapAngle(wbuf[f][l + 2].p[h].a - (adiff1 / 2.0)); 

					double adiff2 = absWrapAngle(wbuf[f][l].p[h].a - cang);
				
					double k2d = k1 * (adiff2 / M_PIl);
				
					y[h] = wbuf[f][l].p[h];

					if (l == 470) {
						//cerr << h << ' ' << cbuf[l].p[h - 2].m << ' ' << cbuf[l - 2].p[h].a << ' ' << cbuf[l].p[h].m << ' ' << cbuf[l].p[h].a << ' ' << cbuf[l].p[h + 2].m << ' ' << cbuf[l + 2].p[h].a  << ' ' << k1 << ' ' << cang << ' ' << adiff2 << ' ' << k2 << endl; 
//						cerr << "2D " << h << ' ' << wbuf[f][l].p[h].m << ' ' << wbuf[f][l].p[h].a << ' ' << k1 << ' ' << cang << ' ' << adiff2 << ' ' << k2d << endl; 
					}

					k = k2d;

					if ((dim == 3) && (framecount > 2)) {
						int adr = (l * 844) + h;
						double k3d = fabs(LPraw[1][adr] - LPraw[0][adr]) + fabs(LPraw[1][adr] - LPraw[2][adr]);

						k3d /= irescale;
						k3d = clamp(1 - ((k3d - 0) / 8), 0, 1);
						
//						cerr << "3D " << h << ' ' << k3d << ' ' << k2d << endl; 

						double mi = ((wbuf[f + 1][l].p[h].i + wbuf[f - 1][l].p[h].i) / 2) * (k3d / 2);
						double mq = ((wbuf[f + 1][l].p[h].q + wbuf[f - 1][l].p[h].q) / 2) * (k3d / 2);
						
						if (l == 475) {
							//cerr << h << ' ' << y[h].m << ' ' << y[h].a; 
							cerr << h << " 1:" << y[h].m << ' ' << y[h].a; 
						}

						y[h].i *= (1 - (k3d / 2));
						y[h].q *= (1 - (k3d / 2));
						
						y[h].i += mi;
						y[h].q += mq;
						
						y[h].ToPolar();

						if (l == 475) {
							double m = ctor(mi, mq); 
							double a = atan2(mq, mi); 
//							cerr << ' ' << k3d << ' ' << mi << ' ' << mq << endl; 
							cerr << ' ' << k3d << ' ' << m << ' ' << a << " 2:" << y[h].m << ' ' << y[h].a << endl; 
						}
//						wbuf[f][l].p[h].i = ((wbuf[f - 1][l].p[h].i + wbuf[f + 1][l].p[h].i) / 4) + (wbuf[f - 1][l].p[h].i / 2); 
//						wbuf[f][l].p[h].q = ((wbuf[f - 1][l].p[h].q + wbuf[f + 1][l].p[h].q) / 4) + (wbuf[f - 1][l].p[h].q / 2); 
						y[h].ToPolar();
					}

					y[h].m *= (1 - k2d);

					//if (l == 470) cerr << h << ' ' << cbuf[l].p[h].i << ' ' << cbuf[l].p[h].q << ' '; 
					y[h].FromPolar();
					cbuf[l].p[h] = y[h];
					//if (l == 470) cerr << cbuf[l].p[h].i << ' ' << cbuf[l].p[h].q << endl; 
				}
			}
		}

		void Do2D(int f) {
			for (int l = 25; l < 504; l++) {
				for (int h = 8; h < 836; h++) {
					double adiff1 = absWrapAngle(wbuf[f][l + 2].p[h].a - wbuf[f][l - 2].p[h].a);
					double k1 = 1.0 - (adiff1 / M_PIl);
					double cang = WrapAngle(wbuf[f][l + 2].p[h].a - (adiff1 / 2.0)); 

					double adiff2 = absWrapAngle(wbuf[f][l].p[h].a - cang);
					double k2 = (adiff2 / M_PIl);

					if (l == 470) {
						//cerr << h << ' ' << cbuf[l].p[h - 2].m << ' ' << cbuf[l - 2].p[h].a << ' ' << cbuf[l].p[h].m << ' ' << cbuf[l].p[h].a << ' ' << cbuf[l].p[h + 2].m << ' ' << cbuf[l + 2].p[h].a  << ' ' << k1 << ' ' << cang << ' ' << adiff2 << ' ' << k2 << endl; 
						cerr << h << ' ' << wbuf[f][l].p[h].m << ' ' << wbuf[f][l].p[h].a << ' ' << k1 << ' ' << cang << ' ' << adiff2 << ' ' << k2 << ' ' << (k1 * k2) << endl; 
					}

					wbuf[f][l].p[h].m *= (1 - (k1 * k2));
				}
			}
			
			for (int l = 25; l < 504; l++) {
				for (int h = 8; h < 836; h++) {
					//if (l == 470) cerr << h << ' ' << cbuf[l].p[h].i << ' ' << cbuf[l].p[h].q << ' '; 
					wbuf[f][l].p[h].FromPolar();
					cbuf[l].p[h] = wbuf[f][l].p[h];
					//if (l == 470) cerr << cbuf[l].p[h].i << ' ' << cbuf[l].p[h].q << endl; 
				}
			}
		}
					
		void DoCNR(int fnum = 0) {
			if (nr_c < 0) return;

			// part 1:  do horizontal 
			for (int l = 24; l < 505; l++) {
				YIQ hpline[844];
				cline_t *input = &cbuf[l];

				for (int h = 70; h < 752 + 70; h++) {
					YIQ y = input->p[h]; 

					hpline[h].i = f_hpi->feed(y.i);
					hpline[h].q = f_hpq->feed(y.q);
				}

				for (int h = 70; h < 744 + 70; h++) {
					YIQ a = hpline[h + 8];

					if (fabs(a.i) < nr_c) {
						double hpm = (a.i / nr_c);
						a.i *= (1 - fabs(hpm * hpm * hpm));
						//a.i -= hpm;
						input->p[h].i -= a.i;
					}
					
					if (fabs(a.q) < nr_c) {
						double hpm = (a.q / nr_c);
						a.q *= (1 - fabs(hpm * hpm * hpm));
						//a.q -= hpm;
						input->p[h].q -= a.q;
					}
				}
			}
		}
		
		void DoYNR() {
			int firstline = (linesout == 505) ? 0 : 24;
			if (nr_y < 0) return;

			// part 1:  do horizontal 
			for (int l = firstline; l < 505; l++) {
				YIQ hpline[844];
				cline_t *input = &cbuf[l];

				for (int h = 70; h < 752 + 70; h++) {
					hpline[h].y = f_hpy->feed(input->p[h].y);
				}

				for (int h = 70; h < 744 + 70; h++) {
					YIQ a = hpline[h + 8];

					if (fabs(a.y) < nr_y) {
						double hpm = (a.y / nr_y);
						a.y *= (1 - fabs(hpm * hpm * hpm));
						input->p[h].y -= a.y;
					}
				}
			}
		}
		
		uint32_t ReadPhillipsCode(uint16_t *line) {
			int first_bit = -1 ;// (int)100 - (1.0 * dots_usec);
			const double bitlen = 2.0 * dots_usec;
			uint32_t out = 0;

			// find first bit
		
			for (int i = 70; (first_bit == -1) && (i < 140); i++) {
				if (u16_to_ire(line[i]) > 90) {
					first_bit = i - (1.0 * dots_usec); 
				}
//				cerr << i << ' ' << line[i] << ' ' << u16_to_ire(line[i]) << ' ' << first_bit << endl;
			}
			if (first_bit < 0) return 0;

			for (int i = 0; i < 24; i++) {
				double val = 0;
	
			//	cerr << dots_usec << ' ' << (int)(first_bit + (bitlen * i) + dots_usec) << ' ' << (int)(first_bit + (bitlen * (i + 1))) << endl;	
				for (int h = (int)(first_bit + (bitlen * i) + dots_usec); h < (int)(first_bit + (bitlen * (i + 1))); h++) {
//					cerr << h << ' ' << line[h] << ' ' << endl;
					val += u16_to_ire(line[h]); 
				}

//				cerr << "bit " << 23 - i << " " << val / dots_usec << ' ' << hex << out << dec << endl;	
				if ((val / dots_usec) > 50) {
					out |= (1 << (23 - i));
				} 
			}
			cerr << "P " << curline << ' ' << hex << out << dec << endl;			

			return out;
		}

	public:
		Comb() {
			fieldcount = curline = linecount = -1;
			framecode = framecount = frames_out = 0;

			scount = 0;

			f_oddframe = false;	
	
			f_i = new Filter(f_colorlp4);
			f_q = new Filter(f_colorlp4);
			
			f_wi = new Filter(f_colorwlp4);
			f_wq = new Filter(f_colorwlp4);

			f_hpy = new Filter(f_nr);
			f_hpi = new Filter(f_nrc);
			f_hpq = new Filter(f_nrc);
			
			f_hpvy = new Filter(f_nr);
			f_hpvi = new Filter(f_nrc);
			f_hpvq = new Filter(f_nrc);
		}

		void WriteFrame(uint16_t *obuf, int fnum = 0) {
			cerr << "WR" << fnum << endl;
			if (!image_mode) {
				write(ofd, obuf, (744 * linesout * 3) * 2);
			} else {
				char ofname[512];

				sprintf(ofname, "%s%d.rgb", image_base, fnum); 
				cerr << "W " << ofname << endl;
				ofd = open(ofname, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IROTH);
				write(ofd, obuf, (744 * linesout * 3) * 2);
				close(ofd);
			}
			if (f_oneframe) exit(0);
		}
		
		// buffer: 844x505 uint16_t array
		void Process(uint16_t *buffer, int dim = 2)
		{
			int firstline = (linesout == 505) ? 1 : 24;
			int f = (dim == 3) ? 1 : 0;

			cerr << "P " << f << ' ' << dim << endl;

			memcpy(wbuf[2], wbuf[1], sizeof(cline_t) * 505);
			memcpy(wbuf[1], wbuf[0], sizeof(cline_t) * 505);

			memcpy(rawbuffer[2], rawbuffer[1], (844 * 505 * 2));
			memcpy(rawbuffer[1], rawbuffer[0], (844 * 505 * 2));
			memcpy(rawbuffer[0], buffer, (844 * 505 * 2));
			
			memcpy(LPraw[2], LPraw[1], (844 * 505 * sizeof(double)));
			memcpy(LPraw[1], LPraw[0], (844 * 505 * sizeof(double)));
		
			memcpy(prevbuf, cbuf, sizeof(cbuf));
	
			LPFrame(0);
			Split(0); 
		
			if ((dim == 3) && (framecount < 2)) {
				framecount++;
				return;
			} 
	
		//	DoCNR(0);	
			//Do2D((dim == 3) ? 1 : 0); 
			DoComb(dim, (dim == 3) ? 1 : 0); 
			
			// remove color data from baseband (Y)	
			for (int l = firstline; l < 505; l++) {
				bool invertphase = (rawbuffer[f][l * 844] == 16384);

				for (int h = 0; h < 760; h++) {
					double comp;	
					int phase = h % 4;

					YIQ y = cbuf[l].p[h + 70];

					switch (phase) {
						case 0: comp = y.i; break;
						case 1: comp = -y.q; break;
						case 2: comp = -y.i; break;
						case 3: comp = y.q; break;
						default: break;
					}

					if (invertphase) comp = -comp;
					y.y += comp;

					cbuf[l].p[h + 70] = y;
				}
			}
			
			DoYNR();
		
			// YIQ (YUV?) -> RGB conversion	
			for (int l = firstline; l < 505; l++) {
				uint16_t *line_output = &output[(744 * 3 * (l - firstline))];
				int o = 0;
				for (int h = 0; h < 752; h++) {
					RGB r;
					YIQ yiq = cbuf[l].p[h + 70];

					if (0 && framecount > 2) {
	
						double k = K3D[(l * 844) + (h + 70)] * .5; 

						// XXX: fix operator override

//						yiq *= (1 - k);
						yiq.y *= (1 - k);
						yiq.i *= (1 - k);
						yiq.q *= (1 - k);
						yiq.y += (prevbuf[l].p[h + 70].y * k);
						yiq.i += (prevbuf[l].p[h + 70].i * k);
						yiq.q += (prevbuf[l].p[h + 70].q * k);
					}
	
					// for debug: display difference areas	
					if (0) {
						int adr = (l * 844) + (h + 70);
               	                                 double k = fabs(LPraw[1][adr] - LPraw[0][adr]) + fabs(LPraw[1][adr] - LPraw[2][adr]);
//            	                                   k /= irescale;
//						cerr << k << ' ' << yiq.y << endl;
						yiq.y = k;
						yiq.i = yiq.q = 0;
					}
	
					r.conv(yiq);
	
      	                                if (0 && l == 475) {
                                               double y = u16_to_ire(yiq.y);
                                               double i = (yiq.i) * (160.0 / 65533.0);
                                               double q = (yiq.q) * (160.0 / 65533.0);

                                               double m = ctor(q, i);
                                               double a = atan2(q, i);

                                               a *= (180 / M_PIl);
                                               if (a < 0) a += 360;

						double k = K3D[(l * 844) + (h + 70)] * .5; 
                                               cerr << h << ' ' << k << ' ' << y << ' ' << i << ' ' << q << ' ' << m << ' ' << a << ' '; 
                                               cerr << r.r << ' ' << r.g << ' ' << r.b << endl;
                                       }

					line_output[o++] = (uint16_t)(r.r); 
					line_output[o++] = (uint16_t)(r.g); 
					line_output[o++] = (uint16_t)(r.b); 
				}
			}

			PostProcess(f);
			framecount++;

			return;
		}

		int PostProcess(int fnum) {
			int fstart = -1;

			if (!pulldown_mode) {
				fstart = 0;
			} else if (f_oddframe) {
				for (int i = 0; i < linesout; i += 2) {
					memcpy(&obuf[744 * 3 * i], &output[744 * 3 * i], 744 * 3 * 2); 
				}
				WriteFrame(obuf, framecode);
				f_oddframe = false;		
			}

			for (int line = 4; line <= 5; line++) {
				int wc = 0;
				for (int i = 0; i < 700; i++) {
					if (rawbuffer[fnum][(844 * line) + i] > 45000) wc++;
				} 
				if (wc > 500) {
					fstart = (line % 2); 
				}
				//cerr << "PW" << fnum << ' ' << line << ' ' << wc << ' ' << fieldcount << endl;
			}

			for (int line = 16; line < 20; line++) {
				int new_framecode = ReadPhillipsCode(&rawbuffer[fnum][line * 844]); // - 0xf80000;
				int fca = new_framecode & 0xf80000;

				//cerr << "c" << line << ' ' << hex << ' ' <<  new_framecode << ' ' << fca << ' ' << dec << endl;

				if (((new_framecode & 0xf00000) == 0xf00000) && (new_framecode < 0xff0000)) {
					int ofstart = fstart;

					framecode = new_framecode & 0x0f;
					framecode += ((new_framecode & 0x000f0) >> 4) * 10;
					framecode += ((new_framecode & 0x00f00) >> 8) * 100;
					framecode += ((new_framecode & 0x0f000) >> 12) * 1000;
					framecode += ((new_framecode & 0xf0000) >> 16) * 10000;

					if (framecode > 80000) framecode -= 80000;
	
					cerr << "frame " << framecode << endl;
	
					fstart = (line % 2); 
					if ((ofstart >= 0) && (fstart != ofstart)) {
						cerr << "MISMATCH\n";
					}
				}
			}

			cerr << "FR " << framecount << ' ' << fstart << endl;
			if (!pulldown_mode || (fstart == 0)) {
//				memcpy(obuf, output, sizeof(output));
				WriteFrame(output, framecode);
			} else if (fstart == 1) {
				for (int i = 1; i < linesout; i += 2) {
					memcpy(&obuf[744 * 3 * i], &output[744 * 3 * i], 744 * 3 * 2); 
				}
//				memcpy(obuf, output, sizeof(output));
				f_oddframe = true;
			}

			return 0;
		}
};
	
Comb comb;

void usage()
{
	cerr << "comb: " << endl;
	cerr << "-i [filename] : input filename (default: stdin)\n";
	cerr << "-o [filename] : output filename/base (default: stdout/frame)\n";
	cerr << "-f : use separate file for each frame\n";
	cerr << "-p : use white flag/frame # for pulldown\n";	
	cerr << "-h : this\n";	
}

int main(int argc, char *argv[])
{
	int rv = 0, fd = 0;
	long long dlen = -1, tproc = 0;
	unsigned short inbuf[844 * 525 * 2];
	unsigned char *cinbuf = (unsigned char *)inbuf;
	int c;

	int dim = 2;

	char out_filename[256] = "";

	cerr << std::setprecision(10);
	cerr << argc << endl;
	cerr << strncmp(argv[1], "-", 1) << endl;

	opterr = 0;
	
	while ((c = getopt(argc, argv, "Owvd:Bb:I:w:i:o:fphn:N:")) != -1) {
		switch (c) {
			case 'd':
				sscanf(optarg, "%d", &dim);
				break;
			case 'O':
				f_oneframe = true;
				break;
			case 'v':
				linesout = 505;
				break;
			case 'B':
				bw_mode = true;
				break;
			case 'w':
				cwide_mode = true;
				break;
			case 'b':
				sscanf(optarg, "%lf", &brightness);
				break;
			case 'I':
				sscanf(optarg, "%lf", &black_ire);
				break;
			case 'n':
				sscanf(optarg, "%lf", &nr_y);
				break;
			case 'N':
				sscanf(optarg, "%lf", &nr_c);
				break;
			case 'h':
				usage();
				return 0;
			case 'f':
				image_mode = true;	
				break;
			case 'p':
				pulldown_mode = true;	
				break;
			case 'i':
				fd = open(optarg, O_RDONLY);
				break;
			case 'o':
				image_base = (char *)malloc(strlen(optarg) + 1);
				strncpy(image_base, optarg, strlen(optarg));
				break;
			default:
				return -1;
		} 
	} 

	black_u16 = ire_to_u16(black_ire);
	cerr << ' ' << black_u16 << endl;

	nr_y = nr_y * irescale;
	nr_c = nr_c * irescale;

	if (!image_mode && strlen(out_filename)) {
		ofd = open(image_base, O_WRONLY | O_CREAT);
	}

	cout << std::setprecision(8);

	int bufsize = 844 * 505 * 2;

	rv = read(fd, inbuf, bufsize);
	while ((rv > 0) && (rv < bufsize)) {
		int rv2 = read(fd, &cinbuf[rv], bufsize - rv);
		if (rv2 <= 0) exit(0);
		rv += rv2;
	}

	while (rv == bufsize && ((tproc < dlen) || (dlen < 0))) {
		comb.Process(inbuf, dim);
	
		rv = read(fd, inbuf, bufsize);
		while ((rv > 0) && (rv < bufsize)) {
			int rv2 = read(fd, &cinbuf[rv], bufsize - rv);
			if (rv2 <= 0) exit(0);
			rv += rv2;
		}
	}

	return 0;
}

