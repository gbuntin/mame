// license:BSD-3-Clause
// copyright-holders:Tomasz Slanina, Pierpaolo Prazzoli
/*
Dynamic Dice (??)

Driver by
    Tomasz Slanina
    Pierpaolo Prazzoli
--
Old, rusty, not working pcb :


Main PCB :

m5l8080ap (8080)

dy_1.bin (1H)
dy_2.bin (1L)
dy_3.bin (1P)

dip 6x  (all off)

xtal 18.432 mhz

--
Sub PCB  DYNA-529-81ST :

AY-3-8910
Z80

dy_4.bin
dy_5.bin
dy_6.bin (near Z80)
*/

#include "emu.h"
#include "cpu/i8085/i8085.h"
#include "cpu/z80/z80.h"
#include "machine/gen_latch.h"
#include "machine/nvram.h"
#include "sound/ay8910.h"
#include "emupal.h"
#include "screen.h"
#include "speaker.h"
#include "tilemap.h"


namespace {

class dynadice_state : public driver_device
{
public:
	dynadice_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_videoram(*this, "videoram"),
		m_maincpu(*this, "maincpu"),
		m_gfxdecode(*this, "gfxdecode"),
		m_ay8910(*this, "ay8910")
	{ }

	void dynadice(machine_config &config);

	void init_dynadice();

protected:
	virtual void machine_start() override ATTR_COLD;
	virtual void machine_reset() override ATTR_COLD;
	virtual void video_start() override ATTR_COLD;

private:
	/* memory pointers */
	required_shared_ptr<uint8_t> m_videoram;
//  uint8_t *  m_nvram;     // currently this uses generic nvram handling

	required_device<cpu_device> m_maincpu;
	required_device<gfxdecode_device> m_gfxdecode;
	required_device<ay8910_device> m_ay8910;

	/* video-related */
	tilemap_t  *m_bg_tilemap = nullptr;
	tilemap_t  *m_top_tilemap = nullptr;

	/* misc */
	int      m_ay_data = 0;

	void videoram_w(offs_t offset, uint8_t data);
	void sound_data_w(uint8_t data);
	void sound_control_w(uint8_t data);

	TILE_GET_INFO_MEMBER(get_tile_info);

	uint32_t screen_update(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect);

	void dynadice_io_map(address_map &map) ATTR_COLD;
	void dynadice_map(address_map &map) ATTR_COLD;
	void dynadice_sound_io_map(address_map &map) ATTR_COLD;
	void dynadice_sound_map(address_map &map) ATTR_COLD;
};


void dynadice_state::videoram_w(offs_t offset, uint8_t data)
{
	m_videoram[offset] = data;
	m_bg_tilemap->mark_tile_dirty(offset);
	m_top_tilemap->mark_all_dirty();
}

void dynadice_state::sound_data_w(uint8_t data)
{
	m_ay_data = data;
}

void dynadice_state::sound_control_w(uint8_t data)
{
/*
    AY 3-8910 :

    D0 - BC1
    D1 - BC2
    D2 - BDIR
    D3 - /Reset

*/
	if ((data & 7) == 7)
		m_ay8910->address_w(m_ay_data);

	if ((data & 7) == 6)
		m_ay8910->data_w(m_ay_data);
}


void dynadice_state::dynadice_map(address_map &map)
{
	map(0x0000, 0x1fff).rom();
	map(0x2000, 0x23ff).ram().w(FUNC(dynadice_state::videoram_w)).share("videoram");
	map(0x4000, 0x40ff).ram().share("nvram");
}

void dynadice_state::dynadice_io_map(address_map &map)
{
	map(0x50, 0x50).portr("IN0");
	map(0x51, 0x51).portr("IN1");
	map(0x52, 0x52).portr("DSW");
	map(0x62, 0x62).nopw();
	map(0x63, 0x63).w("soundlatch", FUNC(generic_latch_8_device::write));
	map(0x70, 0x77).nopw();
}

void dynadice_state::dynadice_sound_map(address_map &map)
{
	map(0x0000, 0x07ff).rom();
	map(0x2000, 0x23ff).ram();
}

void dynadice_state::dynadice_sound_io_map(address_map &map)
{
	map.global_mask(0xff);
	map(0x00, 0x00).r("soundlatch", FUNC(generic_latch_8_device::read));
	map(0x01, 0x01).w("soundlatch", FUNC(generic_latch_8_device::clear_w));
	map(0x02, 0x02).w(FUNC(dynadice_state::sound_data_w));
	map(0x03, 0x03).w(FUNC(dynadice_state::sound_control_w));
}

static INPUT_PORTS_START( dynadice )
	PORT_START("IN0")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_COIN1 )
	PORT_DIPNAME( 0x02, 0x02, "Initialize NVRAM" )
	PORT_DIPSETTING(    0x02, DEF_STR( Off ) )
	PORT_DIPSETTING(    0x00, DEF_STR( On ) )
	PORT_BIT( 0x30, IP_ACTIVE_LOW, IPT_UNKNOWN )

	PORT_START("IN1")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_BUTTON1 ) /* increase number of coins */
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_BUTTON2 ) /* decrease number of coins */
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_START1 )  /* start /stop */

	PORT_START("DSW")
	PORT_DIPNAME( 0x1c, 0x1c, DEF_STR( Coinage ) )
	PORT_DIPSETTING(    0x00, DEF_STR( 5C_1C ))
	PORT_DIPSETTING(    0x04, DEF_STR( 4C_1C ))
	PORT_DIPSETTING(    0x08, DEF_STR( 3C_1C ))
	PORT_DIPSETTING(    0x0c, DEF_STR( 2C_1C ))
	PORT_DIPSETTING(    0x1c, DEF_STR( 1C_1C ))
	PORT_DIPSETTING(    0x18, DEF_STR( 1C_2C ))
	PORT_DIPSETTING(    0x14, DEF_STR( 1C_3C ))
	PORT_DIPSETTING(    0x10, DEF_STR( 1C_4C ))

	PORT_DIPNAME( 0x01, 0x01, "DSW 1-0" )
	PORT_DIPSETTING(    0x01, DEF_STR( Off ) )
	PORT_DIPSETTING(    0x00, DEF_STR( On ) )
	PORT_DIPNAME( 0x02, 0x02, "DSW 1-1" )
	PORT_DIPSETTING(    0x02, DEF_STR( Off ) )
	PORT_DIPSETTING(    0x00, DEF_STR( On ) )
	PORT_DIPNAME( 0x20, 0x20, "DSW 1-5" )
	PORT_DIPSETTING(    0x20, DEF_STR( Off ) )
	PORT_DIPSETTING(    0x00, DEF_STR( On ) )
	PORT_DIPNAME( 0x40, 0x40, "DSW 1-6" )
	PORT_DIPSETTING(    0x40, DEF_STR( Off ) )
	PORT_DIPSETTING(    0x00, DEF_STR( On ) )
	PORT_DIPNAME( 0x80, 0x80, "DSW 1-7" )
	PORT_DIPSETTING(    0x80, DEF_STR( Off ) )
	PORT_DIPSETTING(    0x00, DEF_STR( On ) )
INPUT_PORTS_END

static const gfx_layout charlayout2 =
{
	8,8,
	RGN_FRAC(1,1),
	3,
	{ 5,6,7 },
	{ 0, 1*8, 2*8, 3*8, 4*8, 5*8, 6*8, 7*8 },
	{ 0*8*8, 1*8*8, 2*8*8, 3*8*8, 4*8*8, 5*8*8, 6*8*8, 7*8*8 },
	8*8*8
};


static GFXDECODE_START( gfx_dynadice )
	GFXDECODE_ENTRY( "gfx1", 0, gfx_8x8x1,    0, 1 ) /* 1bpp */
	GFXDECODE_ENTRY( "gfx2", 0, charlayout2,  0, 1 ) /* 3bpp */
GFXDECODE_END

TILE_GET_INFO_MEMBER(dynadice_state::get_tile_info)
{
	int code = m_videoram[tile_index];
	tileinfo.set(1, code, 0, 0);
}

void dynadice_state::video_start()
{
	/* pacman - style videoram layout */
	m_bg_tilemap = &machine().tilemap().create(*m_gfxdecode, tilemap_get_info_delegate(*this, FUNC(dynadice_state::get_tile_info)), TILEMAP_SCAN_ROWS, 8, 8, 32, 32);
	m_top_tilemap = &machine().tilemap().create(*m_gfxdecode, tilemap_get_info_delegate(*this, FUNC(dynadice_state::get_tile_info)), TILEMAP_SCAN_COLS, 8, 8, 2, 32);
	m_bg_tilemap->set_scrollx(0, -16);
}

uint32_t dynadice_state::screen_update(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	rectangle myclip = cliprect;
	myclip.max_x = 15;
	m_bg_tilemap->draw(screen, bitmap, cliprect, 0, 0);
	m_top_tilemap->draw(screen, bitmap, myclip, 0, 0);
	return 0;
}

void dynadice_state::machine_start()
{
	save_item(NAME(m_ay_data));
}

void dynadice_state::machine_reset()
{
	m_ay_data = 0;
}

void dynadice_state::dynadice(machine_config &config)
{
	/* basic machine hardware */
	I8080(config, m_maincpu, 18.432_MHz_XTAL / 8);
	m_maincpu->set_addrmap(AS_PROGRAM, &dynadice_state::dynadice_map);
	m_maincpu->set_addrmap(AS_IO, &dynadice_state::dynadice_io_map);

	z80_device &audiocpu(Z80(config, "audiocpu", 18.432_MHz_XTAL / 6));
	audiocpu.set_addrmap(AS_PROGRAM, &dynadice_state::dynadice_sound_map);
	audiocpu.set_addrmap(AS_IO, &dynadice_state::dynadice_sound_io_map);


	NVRAM(config, "nvram", nvram_device::DEFAULT_ALL_0);

	/* video hardware */
	screen_device &screen(SCREEN(config, "screen", SCREEN_TYPE_RASTER));
	screen.set_refresh_hz(60);
	screen.set_vblank_time(ATTOSECONDS_IN_USEC(0));
	screen.set_size(256+16, 256);
	screen.set_visarea(0*8, 34*8-1, 3*8, 28*8-1);
	screen.set_screen_update(FUNC(dynadice_state::screen_update));
	screen.set_palette("palette");

	GFXDECODE(config, m_gfxdecode, "palette", gfx_dynadice);
	PALETTE(config, "palette", palette_device::BRG_3BIT);

	SPEAKER(config, "mono").front_center();

	GENERIC_LATCH_8(config, "soundlatch");

	AY8910(config, m_ay8910, 2000000).add_route(ALL_OUTPUTS, "mono", 1.0);
}

ROM_START( dynadice )
	ROM_REGION( 0x10000, "maincpu", 0 )
	ROM_LOAD( "dy_1.bin",     0x0000, 0x1000, CRC(4ad18724) SHA1(78151b02a727f4272eff72765883df9ca09606c3) )
	ROM_LOAD( "dy_2.bin",     0x1000, 0x0800, CRC(82cb1873) SHA1(661f33af4a536b7929d432d755ab44f9280f82db) )
	ROM_LOAD( "dy_3.bin",     0x1800, 0x0800, CRC(a8edad20) SHA1(b812141f216355c986047969326bd1e036be71e6) )

	ROM_REGION( 0x10000, "audiocpu", 0 )
	ROM_LOAD( "dy_6.bin",     0x0000, 0x0800, CRC(d4e6e6a3) SHA1(84c0fcfd8326a4301accbd192df6e372b98ae537) )

	ROM_REGION( 0x0800, "gfx1", 0 )
	ROM_LOAD( "dy_4.bin",     0x0000, 0x0800, CRC(306b851b) SHA1(bf69ed126d32b31e1711ff23c5a75b8a8bd28207) )

	ROM_REGION( 0x0800*8, "gfx2", ROMREGION_ERASE00 )
	/* gfx data will be rearranged here for 8x8 3bpp tiles */

	ROM_REGION( 0x0800, "user1",0 )
	ROM_LOAD( "dy_5.bin",     0x0000, 0x0800, CRC(e4799462) SHA1(5cd0f003572540522d72706bc5a8fa6588553031) )
ROM_END

void dynadice_state::init_dynadice()
{
	uint8_t *usr1 = memregion("user1")->base();
	uint8_t *cpu2 = memregion("audiocpu")->base();
	uint8_t *gfx1 = memregion("gfx1")->base();
	uint8_t *gfx2 = memregion("gfx2")->base();

	cpu2[0x0b] = 0x23;  /* bug in game code  Dec HL -> Inc HL*/

	/* 1bpp tiles -> 3bpp tiles (dy_5.bin  contains bg/fg color data for each tile line) */
	for (int i = 0; i < 0x800; i++)
		for (int j = 0; j < 8; j++)
			gfx2[(i << 3) + j] = (gfx1[i] & (0x80 >> j)) ? (usr1[i] & 7) : (usr1[i] >> 4);
}

} // anonymous namespace


GAME( 19??, dynadice, 0, dynadice, dynadice, dynadice_state, init_dynadice, ROT90, "<unknown>", "Dynamic Dice", MACHINE_SUPPORTS_SAVE )
