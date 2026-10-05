#pragma once
struct glModelPacket;
namespace mscharged::detail
{
// Internal callback-free bridge between checked movie ownership and the actual
// selected material draw. Only one movie texture triplet can exist at a time.
void BeginMovieDrawObservation(const glModelPacket*);
bool FinishMovieDrawObservation(const glModelPacket*);
void BeginMoviePacketDraw(const glModelPacket*);
void EndMoviePacketDraw(const glModelPacket*);
}
namespace mscharged::detail
{
// Selected movie-only metadata queries; no broad glTextureLoad service claim.
bool LoadMovieTexture(unsigned long hash);
unsigned MovieTextureWidth();
unsigned MovieTextureHeight();
}
